#include "gpu.h"

#include "heap.h"
#include "debug.h"
#include "wm.h"

typedef struct gpu_cmd_buff_t {
	VkCommandBuffer buffer;
	VkPipelineLayout pipeline_layout;
	int idx_count;
	int vtx_count;
} gpu_cmd_buff_t;

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
	uint32_t frame_index;

	heap_t* heap;
} gpu_t;

static uint32_t gpuGetMemoryTypeIndex(gpu_t* gpu, uint32_t bits, VkMemoryPropertyFlags property_flags);
static void gpuCreateMeshLayouts(gpu_t* gpu);
static void gpuDestroyMeshLayouts(gpu_t* gpu);

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

	float queue_priorities = 0.0f;

	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = queue_family_idx,
		.queueCount = queue_count,
		.pQueuePriorities = &queue_priorities
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
	if(gpu->inst)
		vkDestroyInstance(gpu->inst, NULL);
}

void gpuCommandBind(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff, gpu_mesh_t* mesh) {

}

void gpuCommandDraw(gpu_t* gpu, gpu_cmd_buff_t* cmd_buff) {
	if (cmd_buff->idx_count) {
		vkCmdDrawIndexed(cmd_buff->buffer, cmd_buff->idx_count, 1, 0, 0, 0);
	} else {
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

void* gpuError(gpu_t* gpu, const char* fn_name, const char* reason) {
	debugPrint(DEBUG_PRINT_ERROR, "%s: %s\n", fn_name, reason);
	gpuDestroy(gpu);
	return NULL;
}
