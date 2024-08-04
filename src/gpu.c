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

	gpu_frame_t* frames;
	uint32_t frame_count;
	uint32_t frame_index;

	// memory
	heap_t* heap;
} gpu_t;

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


}

void gpuDestroy(gpu_t* gpu) {
	if(gpu->inst)
		vkDestroyInstance(gpu->inst, NULL);
}

void* gpuError(gpu_t* gpu, const char* fn_name, const char* reason) {
	debugPrint(DEBUG_PRINT_ERROR, "%s: %s\n", fn_name, reason);
	gpuDestroy(gpu);
	return NULL;
}
