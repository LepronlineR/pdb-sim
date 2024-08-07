#include "gpu.h"

#include "heap.h"
#include "debug.h"
#include "wm.h"

#include <malloc.h>

typedef struct gpu_cmd_buff_t {
	VkCommandBuffer buffer;
	VkPipelineLayout pipeline_layout;
	int idx_count;
	int vtx_count;
} gpu_cmd_buff_t;

typedef struct gpu_pipeline_t {
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
} gpu_pipeline_t;

typedef struct gpu_descriptor_t {
	VkDescriptorSet set;
} gpu_descriptor_t;

typedef struct gpu_shader_t {
	VkShaderModule vtx_module;
	VkShaderModule frag_module;
	VkDescriptorSetLayout descriptor_set_layout;
} gpu_shader_t;

typedef struct gpu_uniform_buffer_t {
	VkBuffer buffer;
	VkDeviceMemory dev_mem;
	VkDescriptorBufferInfo descriptor;
} gpu_uniform_buffer_t;

typedef struct gpu_mesh_t {
	VkBuffer idx_buff;
	VkDeviceMemory idx_mem;
	int idx_count;
	VkIndexType idx_type;

	VkBuffer vtx_buff;
	VkDeviceMemory vtx_mem;
	int vtx_count;
} gpu_mesh_t;

typedef struct gpu_frame_t {
	VkImage img;
	VkImageView view;
	VkFramebuffer frame_buff;
	VkFence fence;
	gpu_cmd_buff_t* cmd_buff;
} gpu_frame_t;

typedef struct gpu_t {
	VkInstance inst;
	VkPhysicalDevice phys_dev;
	VkDevice logic_dev;
	VkPhysicalDeviceMemoryProperties mem_prop;
	VkQueue queue;
	VkSurfaceKHR surface;
	VkSwapchainKHR swap_chain;

	VkRenderPass render_pass;
	VkImage depth_stencil_img;
	VkDeviceMemory depth_stencil_mem;
	VkImageView depth_stencil_view;

	VkCommandPool cmd_pool;
	VkDescriptorPool desc_pool;

	VkSemaphore present_comp_sem;
	VkSemaphore render_comp_sem;

	uint32_t frame_width;
	uint32_t frame_height;

	VkPipelineInputAssemblyStateCreateInfo mesh_input_asm_info[GPU_MESH_LAYOUT_COUNT];
	VkPipelineVertexInputStateCreateInfo mesh_vtx_input_info[GPU_MESH_LAYOUT_COUNT];
	VkIndexType mesh_vtx_size[GPU_MESH_LAYOUT_COUNT];
	VkIndexType mesh_idx_size[GPU_MESH_LAYOUT_COUNT];
	VkIndexType mesh_idx_type[GPU_MESH_LAYOUT_COUNT];

	gpu_frame_t* frames;
	uint32_t frame_count;
	uint32_t frame_idx;

	heap_t* heap;
} gpu_t;

static uint32_t gpuGetMemoryTypeIndex(gpu_t* gpu, uint32_t bits, VkMemoryPropertyFlags property_flags);
static void gpuCreateMeshLayouts(gpu_t* gpu);
static void gpuDestroyMeshLayouts(gpu_t* gpu);

//  --------------------------------------------------------------------------
//								INIT/DESTROY GPU
// 

gpu_t* gpuCreate(heap_t* heap, wm_window_t* window) {
	gpu_t* gpu = heapAlloc(heap, sizeof(gpu_t), 8);
	memset(gpu, 0, sizeof(*gpu));
	gpu->heap = heap;

	//
	// ================== Creating an instance ==================
	// 

	VkApplicationInfo app_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "PBD Sim",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.pEngineName = "Simple Engine",
		.engineVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_2
	};

	bool enable_validation_layer = GetEnvironmentVariableW(L"VK_LAYER_PATH", NULL, 0) > 0;

	const char* vk_extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};

	const char* vk_layers[] = {
		"VK_LAYER_KHRONOS_validation",
	};

	VkInstanceCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app_info,
		.enabledExtensionCount = _countof(vk_extensions),
		.ppEnabledExtensionNames = vk_extensions,
		.enabledLayerCount = enable_validation_layer ? _countof(vk_layers) : 0,
		.ppEnabledLayerNames = vk_layers
	};

	VkResult vk_result = vkCreateInstance(&create_info, NULL, &gpu->inst);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "VkCreateInstance", "Create instance failed.");
	}

	//
	// ================== Look for physical devices ==================
	//

	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	uint32_t device_count = 0;
	vk_result = vkEnumeratePhysicalDevices(gpu->inst, &device_count, NULL);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkEnumeratePhysicalDevices", "Function unexpectedly failed.");
	}
	
	if (device_count == 0) {
		return gpuError(gpu, "vkEnumeratePhysicalDevices", "No devices have been found.");
	}

	VkPhysicalDevice* phys_devices = heapAlloc(heap, sizeof(VkPhysicalDevice) * device_count, 8);
	vk_result = vkEnumeratePhysicalDevices(gpu->inst, &device_count, phys_devices);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkEnumeratePhysicalDevices", "Function unexpectedly failed.");
	}
	// TODO: advanced search of a suitable device (https://vulkan-tutorial.com/Drawing_a_triangle/Setup/Physical_devices_and_queue_families)
	physical_device = phys_devices[0];
	heapFree(heap, phys_devices);
	gpu->phys_dev = physical_device;
	
	//
	// ================== Find queue families ==================
	//
	
	uint32_t queue_family_count = 0;
	uint32_t queue_family_idx = UINT32_MAX;
	uint32_t queue_count = UINT32_MAX;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu->phys_dev, &queue_family_count, NULL);

	if(queue_family_count == 0){
		return gpuError(gpu, "vkGetPhysicalDeviceQueueFamilyProperties", "Unable to find a family.");
	}

	VkQueueFamilyProperties* queue_family = heapAlloc(heap, sizeof(VkQueueFamilyProperties) * queue_family_count, 0);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu->phys_dev, &queue_family_count, queue_family);

	for (uint32_t x = 0; x < queue_family_count; x++) {
		if (queue_family[x].queueCount > 0 &&
			queue_family[x].queueFlags & VK_QUEUE_GRAPHICS_BIT) {

			queue_family_idx = x;
			queue_count = queue_family[x].queueCount;
			break;
		}
	}

	heapFree(heap, queue_family);

	if (queue_family_idx == UINT32_MAX || queue_count == UINT32_MAX) {
		return gpuError(gpu, "queueCount, queueFlags", "Unable to find a device with a graphics queue.");
	}

	//
	// ================== Specifying queues to be created ==================
	//

	float* queue_priorities = _alloca(sizeof(float) * queue_count);

	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = queue_family_idx,
		.queueCount = queue_count,
		.pQueuePriorities = queue_priorities
	};

	const char* device_extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME
	};

	//
	// ================== Creating a logical device ==================
	//

	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
		.enabledExtensionCount = _countof(device_extensions),
		.ppEnabledExtensionNames = device_extensions
	};

	vk_result = vkCreateDevice(gpu->phys_dev, &device_info, NULL, &gpu->logic_dev);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateDevice", "Unable to create device.");
	}

	vkGetPhysicalDeviceMemoryProperties(gpu->phys_dev, &gpu->mem_prop);
	
	// Retrieving queue handles
	vkGetDeviceQueue(gpu->logic_dev, queue_family_idx, 0, &gpu->queue);

	//
	// ================== Creating a window surface for rendering ==================
	//

	VkWin32SurfaceCreateInfoKHR win_surface_info = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = GetModuleHandle(NULL),
		.hwnd = wmGetHWND(window)
	};

	vk_result = vkCreateWin32SurfaceKHR(gpu->inst, &win_surface_info, NULL, &gpu->surface);
	if (vk_result) {
		return gpuError(gpu, "vkCreateWin32SurfaceKHR", "Unable to create window surface.");
	}

	VkSurfaceCapabilitiesKHR surface_cap;
	vk_result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu->phys_dev, gpu->surface, &surface_cap);
	if (vk_result) {
		return gpuError(gpu, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "Unable to get surface capabilities.");
	}

	// set frame window
	gpu->frame_width = surface_cap.currentExtent.width;
	gpu->frame_height = surface_cap.currentExtent.height;

	//
	// ================== Creating a swapchain ==================
	//

	VkSwapchainCreateInfoKHR swapchain_info = {
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = gpu->surface,
		.minImageCount = __max(surface_cap.minImageCount + 1, 3),
		.imageFormat = VK_FORMAT_B8G8R8A8_SRGB,
		.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
		.imageExtent = surface_cap.currentExtent,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.preTransform = surface_cap.currentTransform,
		.imageArrayLayers = 1,
		.imageSharingMode = VK_PRESENT_MODE_FIFO_KHR,
		.presentMode = VK_PRESENT_MODE_FIFO_KHR,
		.clipped = VK_TRUE,
		.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
	};
	vk_result = vkCreateSwapchainKHR(gpu->logic_dev, &swapchain_info, NULL, &gpu->swap_chain);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateSwapchainKHR", "Unable to create a swapchain.");
	}

	vk_result = vkGetSwapchainImagesKHR(gpu->logic_dev, gpu->swap_chain, &gpu->frame_count, NULL);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkGetSwapchainImagesKHR", "Unable to get the swapchain images through the logical device.");
	}

	uint32_t total_frames = sizeof(gpu_frame_t) * gpu->frame_count;
	gpu->frames = heapAlloc(heap, total_frames, 8);
	memset(gpu->frames, 0, total_frames);

	//
	// ================== Creating a image views ==================
	//

	VkImage* images = heapAlloc(heap, sizeof(VkImage) * gpu->frame_count, 8);

	vk_result = vkGetSwapchainImagesKHR(gpu->logic_dev, gpu->swap_chain, &gpu->frame_count, images);

	// create image view info for each image
	for (uint32_t x = 0; x < gpu->frame_count; x++) {
		VkImage image = images[x];
		gpu->frames[x].img = image;

		VkImageViewCreateInfo image_view_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.format = VK_FORMAT_B8G8R8A8_SRGB,
			.components = {
				VK_COMPONENT_SWIZZLE_R,
				VK_COMPONENT_SWIZZLE_G,
				VK_COMPONENT_SWIZZLE_B,
				VK_COMPONENT_SWIZZLE_A
			},
			.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.subresourceRange.levelCount = 1,
			.subresourceRange.layerCount = 1,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.image = image
		};
		vk_result = vkCreateImageView(gpu->logic_dev, &image_view_info, NULL, &gpu->frames[x].view);
		if (vk_result != VK_SUCCESS) {
			return gpuError(gpu, "vkCreateImageView", "Unable to create the image view after giving it the frame view.");
		}
	}

	heapFree(heap, images);

	//
	// ================== Creating a depth buffer image ==================
	//

	VkImageCreateInfo depth_image_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.extent = { surface_cap.currentExtent.width, surface_cap.currentExtent.height },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};

	vk_result = vkCreateImage(gpu->logic_dev, &depth_image_info, NULL, gpu->depth_stencil_img);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateImage", "Unable to create the the depth buffer image.");
	}

	VkMemoryRequirements depth_mem_reqs;
	vkGetImageMemoryRequirements(gpu->logic_dev, gpu->depth_stencil_img, &depth_mem_reqs);

	VkMemoryAllocateInfo depth_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = depth_mem_reqs.size,
		.memoryTypeIndex = gpuGetMemoryTypeIndex(gpu, depth_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	vk_result = vkAllocateMemory(gpu->logic_dev, &depth_alloc_info, NULL, &gpu->depth_stencil_mem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkAllocateMemory", "Unable to allocate memory through the depth buffer.");
	}

	vk_result = vkBindImageMemory(gpu->logic_dev, gpu->depth_stencil_img, gpu->depth_stencil_mem, 0);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkBindImageMemory", "Unable to bind the image memory to the depth stencil image.");
	}

	VkImageViewCreateInfo depth_view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_D32_SFLOAT,
		.subresourceRange = {.levelCount = 1, .layerCount = 1},
		.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
		.image = gpu->depth_stencil_img
	};
	vk_result = vkCreateImageView(gpu->logic_dev, &depth_view_info, NULL, &gpu->depth_stencil_view);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateImageView", "Unable to create the image view for the depth buffer.");
	}

	//
	// ================== Create the render pass (draws to screen) ==================
	//

	VkAttachmentDescription attachments[2] = {
		{
			.format = VK_FORMAT_B8G8R8A8_SRGB,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		},
		{
			.format = VK_FORMAT_D32_SFLOAT,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		}
	};

	VkAttachmentReference color_reference = {
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
	};

	VkAttachmentReference depth_reference = {
		.attachment = 1,
		.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	};

	VkSubpassDescription subpass = {
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_reference,
		.pDepthStencilAttachment = &depth_reference,
	};

	VkSubpassDependency dependencies[2] = {
		{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
		{
			.srcSubpass = 0,
			.dstSubpass = VK_SUBPASS_EXTERNAL,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = 0,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		},
	};

	VkRenderPassCreateInfo render_pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = _countof(attachments),
		.pAttachments = attachments,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = _countof(dependencies),
		.pDependencies = dependencies,
	};
	vk_result = vkCreateRenderPass(gpu->logic_dev, &render_pass_info, NULL, &gpu->render_pass);
	if (vk_result != VK_SUCCESS){
		return gpuError(gpu, "vkCreateRenderPass", "Unable to create render pass.");
	}

	//
	// ================== Create the frame buffer objects ==================
	//

	for (uint32_t x = 0; x < gpu->frame_count; x++) {
		VkImageView view_buffer_attachments[2] = {
			gpu->frames[x].view,
			gpu->depth_stencil_view
		};

		VkFramebufferCreateInfo frame_buffer_info = {
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = gpu->render_pass,
			.attachmentCount = _countof(view_buffer_attachments),
			.width = gpu->frame_width,
			.height = gpu->frame_height,
			.layers = 1
		};
		vk_result = vkCreateFramebuffer(gpu->logic_dev, &frame_buffer_info, NULL, &gpu->frames[x].frame_buff);
		if (vk_result != VK_SUCCESS) {
			return gpuError(gpu, "vkCreateFramebuffer", "Unable to create frame buffer at frame count: %zu.", x);
		}
	}

	//
	// ================== Create semaphores for GPU/CPU sync ==================
	//

	VkSemaphoreCreateInfo semaphore_info = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
	};
	vk_result = vkCreateSemaphore(gpu->logic_dev, &semaphore_info, NULL, &gpu->present_comp_sem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateSemaphore", "Unable to create a semaphore.");
	}
	vk_result = vkCreateSemaphore(gpu->logic_dev, &semaphore_info, NULL, &gpu->render_comp_sem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateSemaphore", "Unable to create a semaphore.");
	}

	//
	// ================== Create descriptor pools ==================
	//
	
	VkDescriptorPoolSize descriptor_pool_sizes[1] = {
		{
			.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 512
		}
	};

	VkDescriptorPoolCreateInfo descriptor_pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.poolSizeCount = _countof(descriptor_pool_sizes),
		.pPoolSizes = descriptor_pool_sizes,
		.maxSets = 512
	};
	vk_result = vkCreateDescriptorPool(gpu->logic_dev, &descriptor_pool_info, NULL, &gpu->desc_pool);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateDescriptorPool", "Unable to create a derscriptor pool.");
	}
	
	//
	// ================== Create command pools ==================
	//

	VkCommandPoolCreateInfo cmd_pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = queue_family_idx,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
	};
	vk_result = vkCreateCommandPool(gpu->logic_dev, &cmd_pool_info, NULL, &gpu->cmd_pool);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateCommandPool", "Unable to create a command pool.");
	}

	//
	// ================== Create command buffer objects ==================
	//
	
	for (uint32_t x = 0; x < gpu->frame_count; x++) {
		gpu->frames[x].cmd_buff = heapAlloc(gpu->heap, sizeof(gpu_cmd_buff_t), 8);
		memset(gpu->frames[x].cmd_buff, 0, sizeof(gpu_cmd_buff_t));

		VkCommandBufferAllocateInfo cmd_buff_alloc_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = gpu->cmd_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1
		};
		vk_result = vkAllocateCommandBuffers(gpu->logic_dev, &cmd_buff_alloc_info, &gpu->frames[x].cmd_buff->buffer);
		if (vk_result != VK_SUCCESS) {
			return gpuError(gpu, "vkAllocateCommandBuffers", "Unable to allocate command buffers for the frame %zu.", x);
		}

		VkFenceCreateInfo fence_info = {
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT
		};
		vk_result = vkCreateFence(gpu->logic_dev, &fence_info, NULL, &gpu->frames[x].fence);
		if (vk_result != VK_SUCCESS) {
			return gpuError(gpu, "vkCreate Fence", "Unable to create a fence for allocating command buffers for the frame %zu.", x);
		}
	}

	gpuCreateMeshLayouts(gpu);

	return gpu;
}

void gpuDestroy(gpu_t* gpu) {
	if (gpu) {
		if (gpu->queue)
			vkQueueWaitIdle(gpu->queue);

		gpuDestroyMeshLayouts(gpu);

		if (gpu->depth_stencil_img)
			vkDestroyImage(gpu->logic_dev, gpu->depth_stencil_img, NULL);
		if (gpu->depth_stencil_view)
			vkDestroyImageView(gpu->logic_dev, gpu->depth_stencil_view, NULL);
		if (gpu->depth_stencil_mem)
			vkFreeMemory(gpu->logic_dev, gpu->depth_stencil_mem, NULL);
			

		if (gpu->present_comp_sem)
			vkDestroySemaphore(gpu->logic_dev, gpu->present_comp_sem, NULL);
		if (gpu->render_comp_sem)
			vkDestroySemaphore(gpu->logic_dev, gpu->render_comp_sem, NULL);

		if (gpu->swap_chain)
			vkDestroySwapchainKHR(gpu->logic_dev, gpu->swap_chain, NULL);

		if (gpu->desc_pool)
			vkDestroyDescriptorPool(gpu->logic_dev, gpu->desc_pool, NULL);
		
		if (gpu->cmd_pool)
			vkDestroyCommandPool(gpu->logic_dev, gpu->cmd_pool, NULL);

		if (gpu->frames) {
			for (uint32_t x = 0; x < gpu->frame_count; x++) {
				gpu_frame_t* frame = &gpu->frames[x];
				if (frame->fence)
					vkDestroyFence(gpu->logic_dev, frame->fence, NULL);
				if (frame->frame_buff)
					vkDestroyFramebuffer(gpu->logic_dev, frame->frame_buff, NULL);
				if (frame->view)
					vkDestroyImageView(gpu->logic_dev, frame->view, NULL);
				if (frame->cmd_buff) {
					vkFreeCommandBuffers(gpu->logic_dev, gpu->cmd_pool, 1, frame->cmd_buff);
					heapFree(gpu->heap, frame->cmd_buff);
				}
			}
			heapFree(gpu->heap, gpu->frames);
		}

		if (gpu->logic_dev)
			vkDestroyDevice(gpu->logic_dev, NULL);

		if (gpu->surface)
			vkDestroySurfaceKHR(gpu->inst, gpu->surface, NULL);

	
		heapFree(gpu->heap, gpu);
	}
}

//  --------------------------------------------------------------------------
//								FRAME UPDATES
// 

gpu_cmd_buff_t* gpuBeginFrameUpdate(gpu_t* gpu) {
	
	gpu_frame_t* frame = &gpu->frames[gpu->frame_idx];

	VkCommandBufferBeginInfo command_buff_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
	};
	VkResult vk_result = vkBeginCommandBuffer(frame->cmd_buff, &command_buff_info);
	if (vk_result != VK_SUCCESS) { // NOTE: as long as this works, everything else should be fine
		return gpuError(gpu, "vkBeginCommandBuffer", "Unable to create a command buffer on begin frame update.");
	}

	VkClearValue clear_value[2] = {
		{.color = {.float32 = { 0.0f, 0.0f, 0.2f, 1.0f }}},
		{.depthStencil = {.depth = 1.0f, .stencil = 0}}
	};

	VkRenderPassBeginInfo render_pass_begin_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = gpu->render_pass,
		.renderArea.extent.height = gpu->frame_height,
		.renderArea.extent.width = gpu->frame_width,
		.clearValueCount = _countof(clear_value),
		.framebuffer = frame->frame_buff
	};

	vkCmdBeginRenderPass(frame->cmd_buff->buffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport viewport = {
		.height = (float) gpu->frame_height,
		.width = (float) gpu->frame_width,
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(frame->cmd_buff->buffer, 0, 1, &viewport);

	VkRect2D scissor = {
		.extent.width = gpu->frame_width,
		.extent.height = gpu->frame_height,
	};
	vkCmdSetScissor(frame->cmd_buff->buffer, 0, 1, &scissor);

	return frame->cmd_buff;
}

void gpuEndFrameUpdate(gpu_t* gpu) {
	gpu_frame_t* frame = &gpu->frames[gpu->frame_idx];
	gpu->frame_idx = (gpu->frame_idx + 1) % gpu->frame_count;

	vkCmdEndRenderPass(frame->cmd_buff->buffer);
	VkResult result = vkEndCommandBuffer(frame->cmd_buff->buffer);
	if (result != VK_SUCCESS) {
		return gpuError(gpu, "vkEndCommandBuffer", "Unable to end command buffer during ending the frame update.");
	}

	uint32_t image_idx;
	result = vkAcquireNextImageKHR(gpu->logic_dev, gpu->swap_chain, UINT64_MAX, gpu->present_comp_sem, VK_NULL_HANDLE, &image_idx);
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		return gpuError(gpu, "vkAcquireNextImageKHR", "Unable to acquire the next image during ending the frame update.");
	}

	vkWaitForFences(gpu->logic_dev, 1, &frame->fence, VK_TRUE, UINT64_MAX);
	vkResetFences(gpu->logic_dev, 1, &frame->fence);

	VkPipelineStageFlags wait_stage_mask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pWaitDstStageMask = &wait_stage_mask,
		.waitSemaphoreCount = 1,
		.signalSemaphoreCount = 1,
		.pCommandBuffers = &frame->cmd_buff->buffer,
		.commandBufferCount = 1,
		.pWaitSemaphores = &gpu->present_comp_sem,
		.pSignalSemaphores = &gpu->render_comp_sem,
	};

	result = vkQueueSubmit(gpu->queue, 1, &submit_info, frame->fence);
	if (result != VK_SUCCESS) {
		return gpuError(gpu, "vkQueueSubmit", "Unable to submit the queue when ending a frame update.");
	}

	VkPresentInfoKHR present_info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.swapchainCount = 1,
		.pSwapchains = &gpu->swap_chain,
		.pImageIndices = &image_idx,
		.pWaitSemaphores = &gpu->render_comp_sem,
		.waitSemaphoreCount = 1
	};
	result = vkQueuePresentKHR(gpu->queue, &present_info);
	if (result != VK_SUCCESS) {
		return gpuError(gpu, "vkQueuePresentKHR", "Unable to present the queue when ending a frame update.");
	}
}

//  --------------------------------------------------------------------------
//								DESCRIPTOR SETS
// 

gpu_descriptor_t* gpuCreateDescriptorSets(gpu_t* gpu, const gpu_descriptor_info_t* descriptor_info) {
	gpu_descriptor_t* descriptor = heapAlloc(gpu->heap, sizeof(gpu_descriptor_t), 8);
	memset(descriptor, 0, sizeof(*descriptor));
	
	VkDescriptorSetAllocateInfo descriptor_set_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = gpu->desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &descriptor_info->shader->descriptor_set_layout
	};
	VkResult vk_result = vkAllocateDescriptorSets(gpu->logic_dev, &descriptor_set_alloc_info, &descriptor->set);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkAllocateDescriptorSets", "Unalbe to allocate for a descriptor set.");
	}

	VkWriteDescriptorSet* write_descriptor_set = _alloca(sizeof(VkWriteDescriptorSet) * descriptor_info->uniform_buffer_count);
	for (int x = 0; x < descriptor_info->uniform_buffer_count; x++) {
		write_descriptor_set[x] = (VkWriteDescriptorSet){
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = descriptor->set,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.pBufferInfo = &descriptor_info->uniform_buffers[x]->descriptor,
			.dstBinding = x
		};
	}
	vkUpdateDescriptorSets(gpu->logic_dev, descriptor_info->uniform_buffer_count, write_descriptor_set, 0, NULL);
	
	return descriptor;
}

void gpuDestroyDescriptorSets(gpu_t* gpu, gpu_descriptor_t* descriptor) {
	if (descriptor) {
		if (descriptor->set)
			vkFreeDescriptorSets(gpu->logic_dev, gpu->desc_pool, 1, &descriptor->set);
		heapFree(gpu->heap, descriptor);
	}
}

void gpuCommandBindDescriptorSets(gpu_cmd_buff_t* cmd_buff, gpu_descriptor_t* descriptor) {
	vkCmdBindDescriptorSets(cmd_buff->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, cmd_buff->pipeline_layout, 0, 1, &descriptor->set, 0, NULL);
}

//  --------------------------------------------------------------------------
//								    PIPELINE
// 

gpu_pipeline_t* gpuCreatePipeline(gpu_t* gpu, const gpu_pipeline_info_t* pipeline_info) {
	gpu_pipeline_t* pipeline = heapAlloc(gpu->heap, sizeof(gpu_pipeline_t), 8);
	memset(pipeline, 0, sizeof(*pipeline));

	VkPipelineRasterizationStateCreateInfo raster_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_CLOCKWISE,
		.lineWidth = 1.0f
	};

	VkPipelineColorBlendAttachmentState color_blend_state = {
		.colorWriteMask = 0xf,
		.blendEnable = VK_FALSE
	};

	VkPipelineColorBlendStateCreateInfo color_blend_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color_blend_state
	};

	VkPipelineViewportStateCreateInfo viewport_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1
	};

	VkPipelineDepthStencilStateCreateInfo depth_stencil_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		.depthBoundsTestEnable = VK_FALSE,
		.back.failOp = VK_STENCIL_OP_KEEP,
		.back.passOp = VK_STENCIL_OP_KEEP,
		.back.compareOp = VK_COMPARE_OP_ALWAYS,
		.stencilTestEnable = VK_FALSE
	};

	VkPipelineMultisampleStateCreateInfo multisample_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
	};

	VkPipelineShaderStageCreateInfo shader_stage_info[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = pipeline_info->shader->vtx_module,
			.pName = "main"
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = pipeline_info->shader->frag_module,
			.pName = "main"
		}
	};

	VkDynamicState dynamic_state[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamic_state_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = _countof(dynamic_state),
		.pDynamicStates = dynamic_state
	};

	VkPipelineLayoutCreateInfo pipeline_layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &pipeline_info->shader->descriptor_set_layout
	};

	VkResult vk_result = vkCreatePipelineLayout(gpu->logic_dev, &pipeline_layout_info, NULL, &pipeline->pipeline_layout);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreatePipelineLayout", "Unable to create a pipeline layout.");
	}

	VkGraphicsPipelineCreateInfo graphics_pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.layout = pipeline->pipeline_layout,
		.pColorBlendState = &color_blend_state_info,
		.pDepthStencilState = &depth_stencil_state_info,
		.pDynamicState = &dynamic_state_info,
		.stageCount = _countof(shader_stage_info),
		.pStages = shader_stage_info,
		.pRasterizationState = &raster_state_info,
		.pColorBlendState = &color_blend_state_info,
		.pViewportState = &viewport_state_info,
		.pVertexInputState = &gpu->mesh_vtx_input_info[pipeline_info->mesh_layout],
		.pInputAssemblyState = &gpu->mesh_input_asm_info[pipeline_info->mesh_layout],
		.pMultisampleState = &multisample_state_info,
		.renderPass = gpu->render_pass,
	};
	vk_result = vkCreateGraphicsPipelines(gpu->logic_dev, NULL, 1, &graphics_pipeline_info, NULL, &pipeline->pipeline);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateGraphicsPipelines", "Unable to create a graphics pipeline.");
	}

	return pipeline;
}

void gpuDestroyPipeline(gpu_t* gpu, gpu_pipeline_t* pipeline) {
	if (pipeline) {
		if (pipeline->pipeline_layout)
			vkDestroyPipelineLayout(gpu->logic_dev, pipeline->pipeline_layout, NULL);
		if (pipeline->pipeline)
			vkDestroyPipeline(gpu->logic_dev, pipeline->pipeline, NULL);

		heapFree(gpu->heap, pipeline);
	}
}

void gpuCommandBindPipeline(gpu_cmd_buff_t* cmd_buff, gpu_pipeline_t* pipeline) {
	vkCmdBindPipeline(cmd_buff->buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
	cmd_buff->pipeline_layout = pipeline->pipeline_layout;
}

//  --------------------------------------------------------------------------
//								 UNIFORM BUFFER
// 

gpu_uniform_buffer_t* gpuCreateUniformBuffer(gpu_t* gpu, const gpu_uniform_buffer_info_t* ub_info) {
	gpu_uniform_buffer_t* ub = heapAlloc(gpu->heap, sizeof(gpu_uniform_buffer_t), 8);
	memset(ub, 0, sizeof(*ub));

	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
		.size = ub_info->size
	};
	VkResult vk_result = vkCreateBuffer(gpu->logic_dev, &buffer_info, NULL, &ub->buffer);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateBuffer", "Unable to create the uniform buffer.");
	}

	VkMemoryRequirements mem_req;
	vkGetBufferMemoryRequirements(gpu->logic_dev, ub->buffer, &mem_req);

	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = gpuGetMemoryTypeIndex(gpu, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	};
	vk_result = vkAllocateMemory(gpu->logic_dev, &mem_alloc, NULL, &ub->dev_mem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkAllocateMemory", "Unable to allocate memory for the uniform buffer.");
	}

	vk_result = vkBindBufferMemory(gpu->logic_dev, ub->buffer, ub->dev_mem, 0);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkBindBufferMemory", "Unable to bind memory for the uniform buffer.");
	}

	ub->descriptor.buffer = ub->buffer;
	ub->descriptor.range = ub_info->size;

	gpuUpdateUniformBuffer(gpu, ub, ub_info->data, ub_info->size);

	return ub;
}

void gpuUpdateUniformBuffer(gpu_t* gpu, gpu_uniform_buffer_t* ub, const void* data, size_t size) {
	void* mem_alloc = NULL;
	VkResult vk_result = vkMapMemory(gpu->logic_dev, ub->dev_mem, 0, size, 0, &mem_alloc);
	if (vk_result == VK_SUCCESS) {
		memcpy(mem_alloc, data, size);
		vkUnmapMemory(gpu->logic_dev, ub->dev_mem);
	}
}

void gpuDestroyUniformBuffer(gpu_t* gpu, gpu_uniform_buffer_t* ub) {
	if (ub) {
		if (ub->buffer)
			vkDestroyBuffer(gpu->logic_dev, ub->buffer, NULL);
		if (ub->dev_mem)
			vkFreeMemory(gpu->logic_dev, ub->dev_mem, NULL);

		heapFree(gpu, ub);
	}
}


//  --------------------------------------------------------------------------
//								      MESH
// 

gpu_mesh_t* gpuCreateMesh(gpu_t* gpu, gpu_mesh_info_t* mesh_info) {
	gpu_mesh_t* mesh = heapAlloc(gpu->heap, sizeof(gpu_mesh_t), 8);
	memset(mesh, 0, sizeof(*mesh));

	mesh->idx_type = gpu->mesh_idx_type[mesh_info->layout];
	mesh->idx_count = (int) mesh_info->idx_data_size / gpu->mesh_idx_size[mesh_info->layout];
	mesh->vtx_count = (int) mesh_info->vtx_data_size / gpu->mesh_vtx_size[mesh_info->layout];
	
	//
	// ================== Vertex Data ==================
	//

	VkBufferCreateInfo vtx_buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = mesh_info->vtx_data_size,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
	};
	VkResult vk_result = vkCreateBuffer(gpu->logic_dev, &vtx_buffer_info, NULL, &mesh->vtx_buff);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateBuffer", "Unable to create the vertex buffer for a mesh.");
	}

	VkMemoryRequirements mem_req;
	vkGetBufferMemoryRequirements(gpu->logic_dev, mesh->vtx_buff, &mem_req);
	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = gpuGetMemoryTypeIndex(gpu, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	};
	vk_result = vkAllocateMemory(gpu->logic_dev, &mem_alloc, NULL, &mesh->vtx_mem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkAllocateMemory", "Unable to allocate memory for a vertex (during mesh).");
	}
	void* mem_dest = 0;
	vk_result = vkMapMemory(gpu->logic_dev, mesh->vtx_mem, 0, mem_alloc.allocationSize, 0, &mem_dest);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkMapMemory", "Unable to map memory");
	}
	memcpy(mem_dest, mesh_info->vtx_data, mesh_info->vtx_data_size);
	vkUnmapMemory(gpu->logic_dev, mesh->vtx_mem);

	vk_result = vkBindBufferMemory(gpu->logic_dev, mesh->vtx_buff, mesh->vtx_mem, 0);
	if (vk_result) {
		return gpuError(gpu, "vkBindBufferMemory", "Unable to bind buffer memory for a vertex (during mesh).");
	}
	
	//
	// ================== Index Data ==================
	//

	VkBufferCreateInfo idx_buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = mesh_info->idx_data_size,
		.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT
	};
	VkResult vk_result = vkCreateBuffer(gpu->logic_dev, &idx_buffer_info, NULL, &mesh->idx_buff);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateBuffer", "Unable to create the index buffer for a mesh.");
	}

	VkMemoryRequirements mem_req;
	vkGetBufferMemoryRequirements(gpu->logic_dev, mesh->idx_buff, &mem_req);
	VkMemoryAllocateInfo mem_alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mem_req.size,
		.memoryTypeIndex = gpuGetMemoryTypeIndex(gpu, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
	};
	vk_result = vkAllocateMemory(gpu->logic_dev, &mem_alloc, NULL, &mesh->idx_mem);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkAllocateMemory", "Unable to allocate memory for an index (during mesh).");
	}
	void* mem_dest = 0;
	vk_result = vkMapMemory(gpu->logic_dev, mesh->idx_mem, 0, mem_alloc.allocationSize, 0, &mem_dest);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkMapMemory", "Unable to map memory");
	}
	memcpy(mem_dest, mesh_info->idx_data, mesh_info->idx_data_size);
	vkUnmapMemory(gpu->logic_dev, mesh->idx_mem);

	vk_result = vkBindBufferMemory(gpu->logic_dev, mesh->idx_buff, mesh->idx_mem, 0);
	if (vk_result) {
		return gpuError(gpu, "vkBindBufferMemory", "Unable to bind buffer memory for an index (during mesh).");
	}

	return mesh;
}

void gpuDestroyMesh(gpu_t* gpu, gpu_mesh_t* mesh) {
	if (mesh) {
		if (mesh->idx_buff)
			vkDestroyBuffer(gpu->logic_dev, mesh->idx_buff, NULL);
		if (mesh->vtx_buff)
			vkDestroyBuffer(gpu->logic_dev, mesh->vtx_buff, NULL);

		if (mesh->idx_mem)
			vkFreeMemory(gpu->logic_dev, mesh->idx_mem, NULL);
		if (mesh->vtx_mem)
			vkFreeMemory(gpu->logic_dev, mesh->vtx_mem, NULL);

		heapFree(gpu->heap, mesh);
	}
}

void gpuCommandBindMesh(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff, gpu_mesh_t* mesh) {
	if (mesh->vtx_count > 0) {
		VkDeviceSize dev_size = 0;
		vkCmdBindVertexBuffers(cmd_buff->buffer, 0, 1, &mesh->vtx_buff, &dev_size);
		cmd_buff->vtx_count = mesh->vtx_count;
	} else {
		cmd_buff->vtx_count = 0;
	}

	if (mesh->idx_count > 0) {
		vkCmdBindIndexBuffer(cmd_buff->buffer, mesh->idx_buff, 0, mesh->idx_type);
		cmd_buff->idx_count = mesh->idx_count;
	} else {
		cmd_buff->idx_count = 0;
	}
}

void gpuCommandDraw(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff) {
	if (cmd_buff->idx_count) {
		vkCmdDrawIndexed(cmd_buff->buffer, cmd_buff->idx_count, 1, 0, 0, 0);
	}
	else {
		vkCmdDraw(cmd_buff->buffer, cmd_buff->vtx_count, 1, 0, 0);
	}
}

static uint32_t gpuGetMemoryTypeIndex(gpu_t* gpu, uint32_t bits, VkMemoryPropertyFlags property_flags) {
	for (uint32_t x = 0; x < gpu->mem_prop.memoryTypeCount; x++) {
		if ((bits & (1UL << x)) &&
			(gpu->mem_prop.memoryTypes[x].propertyFlags & property_flags) == property_flags) {
			return x;
		}
	}
	debugPrint(DEBUG_PRINT_ERROR, "Get Memory Type Index: Unable to find memory of type (%x)\n", bits);
	return 0;
}

//  --------------------------------------------------------------------------
//								    MESH LAYOUT
// 

static void gpuCreateMeshLayouts(gpu_t* gpu) {
	//
	// ================== GPU_MESH_LAYOUT_TRI_P444_I2 ==================
	//

	gpu->mesh_input_asm_info[GPU_MESH_LAYOUT_TRI_P444_I2] = (VkPipelineInputAssemblyStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	};

	VkVertexInputBindingDescription* vertex_binding = heapAlloc(gpu->heap, sizeof(VkVertexInputBindingDescription), 0);
	vertex_binding->binding = 0;
	vertex_binding->stride = 12;
	vertex_binding->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription* vertex_attributes = heapAlloc(gpu->heap, sizeof(VkVertexInputAttributeDescription), 8);
	vertex_attributes[0] = (VkVertexInputAttributeDescription) {
		.binding = 0,
		.location = 0,
		.format = VK_FORMAT_R32G32B32_SFLOAT,
		.offset = 0,
	};

	gpu->mesh_vtx_input_info[GPU_MESH_LAYOUT_TRI_P444_I2] = (VkPipelineVertexInputStateCreateInfo) {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = vertex_binding,
		.vertexAttributeDescriptionCount = 1,
		.pVertexAttributeDescriptions = vertex_attributes,
	};

	gpu->mesh_idx_type[GPU_MESH_LAYOUT_TRI_P444_I2] = VK_INDEX_TYPE_UINT16;
	gpu->mesh_idx_size[GPU_MESH_LAYOUT_TRI_P444_I2] = 2;
	gpu->mesh_vtx_size[GPU_MESH_LAYOUT_TRI_P444_I2] = 12;

	//
	// ================== GPU_MESH_LAYOUT_TRI_P444_C444_I2 ==================
	//

	gpu->mesh_input_asm_info[GPU_MESH_LAYOUT_TRI_P444_C444_I2] = (VkPipelineInputAssemblyStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
	};

	VkVertexInputBindingDescription* vertex_binding = heapAlloc(gpu->heap, sizeof(VkVertexInputBindingDescription), 0);
	vertex_binding->binding = 0;
	vertex_binding->stride = 24;
	vertex_binding->inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription* vertex_attributes = heapAlloc(gpu->heap, 2 * sizeof(VkVertexInputAttributeDescription), 8);
	vertex_attributes[0] = (VkVertexInputAttributeDescription){
		.binding = 0,
		.location = 0,
		.format = VK_FORMAT_R32G32B32_SFLOAT,
		.offset = 0,
	};
	vertex_attributes[1] = (VkVertexInputAttributeDescription){
		.binding = 0,
		.location = 1,
		.format = VK_FORMAT_R32G32B32_SFLOAT,
		.offset = 12,
	};

	gpu->mesh_vtx_input_info[GPU_MESH_LAYOUT_TRI_P444_C444_I2] = (VkPipelineVertexInputStateCreateInfo){
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = vertex_binding,
		.vertexAttributeDescriptionCount = 2,
		.pVertexAttributeDescriptions = vertex_attributes,
	};

	gpu->mesh_idx_type[GPU_MESH_LAYOUT_TRI_P444_C444_I2] = VK_INDEX_TYPE_UINT16;
	gpu->mesh_idx_size[GPU_MESH_LAYOUT_TRI_P444_C444_I2] = 2;
	gpu->mesh_vtx_size[GPU_MESH_LAYOUT_TRI_P444_C444_I2] = 24;
}

static void gpuDestroyMeshLayouts(gpu_t* gpu) {
	for (int x = 0; x < _countof(gpu->mesh_vtx_input_info); x++) {
		if (gpu->mesh_vtx_input_info[x].pVertexAttributeDescriptions) {
			heapFree(gpu->heap, gpu->mesh_vtx_input_info[x].pVertexBindingDescriptions);
			heapFree(gpu->heap, gpu->mesh_vtx_input_info[x].pVertexAttributeDescriptions);
		}
	}
}

//  --------------------------------------------------------------------------
//								    SHADERS
// 

gpu_shader_t* gpuCreateShader(gpu_t* gpu, gpu_shader_info_t* shader_info) {
	gpu_shader_t* shader = heapAlloc(gpu->heap, sizeof(gpu_shader_t), 8);
	memset(shader, 0, sizeof(*shader));

	VkShaderModuleCreateInfo vertex_module_create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shader_info->vtx_shader_size,
		.pCode = shader_info->vtx_shader_data
	};

	VkResult vk_result = vkCreateShaderModule(gpu->logic_dev, &vertex_module_create_info, NULL, &shader->vtx_module);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateShaderModule", "Unable to create the vertex shader module.");
	}

	VkShaderModuleCreateInfo fragment_module_create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = shader_info->frag_shader_size,
		.pCode = shader_info->frag_shader_data
	};

	vk_result = vkCreateShaderModule(gpu->logic_dev, &fragment_module_create_info, NULL, &shader->frag_module);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateShaderModule", "Unable to create the fragment shader module.");
	}

	VkDescriptorSetLayoutBinding* descriptor_set_layout_bindings = alloca(sizeof(VkDescriptorSetLayoutBinding) * shader_info->uniform_buffer_count);
	for (int x = 0; x < shader_info->uniform_buffer_count; x++) {
		descriptor_set_layout_bindings[x] = (VkDescriptorSetLayoutBinding){
			.binding = x,
			.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			.descriptorCount = 1,
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
		};
	}

	VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = shader_info->uniform_buffer_count,
		.pBindings = descriptor_set_layout_bindings
	};
	vk_result = vkCreateDescriptorSetLayout(gpu->logic_dev, &descriptor_set_layout_info, NULL, &shader->descriptor_set_layout);
	if (vk_result != VK_SUCCESS) {
		return gpuError(gpu, "vkCreateDescriptorSetLayout", "Unable to create the descriptor set layout.");
	}

	return shader;
}

void gpuDestroyShader(gpu_t* gpu, gpu_shader_t* shader) {
	if (shader) {
		if (shader->vtx_module)
			vkDestroyShaderModule(gpu->logic_dev, shader->vtx_module, NULL);
		if (shader->frag_module)
			vkDestroyShaderModule(gpu->logic_dev, shader->frag_module, NULL);
		if (shader->descriptor_set_layout)
			vkDestroyDescriptorSetLayout(gpu->logic_dev, shader->descriptor_set_layout, NULL);
		heapFree(gpu->heap, shader);
	}
}

//  --------------------------------------------------------------------------
//								    MISC
// 

void gpuQueueWaitIdle(gpu_t* gpu) {
	vkQueueWaitIdle(gpu->queue);
}

uint32_t gpuGetFrameCount(gpu_t* gpu) {
	return gpu->frame_count;
}

void* gpuError(gpu_t* gpu, const char* fn_name, const char* reason) {
	debugPrint(DEBUG_PRINT_ERROR, "%s: %s\n", fn_name, reason);
	gpuDestroy(gpu);
	return NULL;
}

