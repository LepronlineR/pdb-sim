#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
#include "gpu.h"
#include "heap.h"
#include "mutex.h"
#include "ui.h"
#include "wm.h"
}

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND window, UINT message, WPARAM w_param, LPARAM l_param);

struct ui_t {
	heap_t* heap;
	wm_window_t* window;
	mutex_t* mutex;
	ImGuiContext* context;
	ui_ball_info_t ball_info;
	bool initialized;
	bool spawn_pending;
};

static ui_t* active_ui;

extern "C" void uiCheckVulkanResult(VkResult result) {
	(void)result;
}

extern "C" ui_t* uiCreate(heap_t* heap, wm_window_t* window) {
	ui_t* ui = (ui_t*)heapAlloc(heap, sizeof(ui_t), 8);
	ui->heap = heap;
	ui->window = window;
	ui->mutex = mutexCreate();
	ui->context = nullptr;
	ui->ball_info.position[0] = 0.0f;
	ui->ball_info.position[1] = 8.0f;
	ui->ball_info.position[2] = 0.0f;
	ui->ball_info.color[0] = 0.85f;
	ui->ball_info.color[1] = 0.35f;
	ui->ball_info.color[2] = 0.55f;
	ui->ball_info.radius = 1.0f;
	ui->ball_info.mass = 1.0f;
	ui->ball_info.structural_compliance = 3.0e-5f;
	ui->ball_info.bending_compliance = 2.0e-4f;
	ui->ball_info.volume_compliance = 1.0e-8f;
	ui->ball_info.velocity_retention = 0.90f;
	ui->initialized = false;
	ui->spawn_pending = false;
	active_ui = ui;
	wmSetCursorCaptured(window, false);
	wmSetMessageCallback(window, uiWindowMessage, ui);
	return ui;
}

extern "C" void uiDestroy(ui_t* ui) {
	if (!ui) return;
	wmSetMessageCallback(ui->window, nullptr, nullptr);
	if (active_ui == ui) active_ui = nullptr;
	mutexDestroy(ui->mutex);
	heapFree(ui->heap, ui);
}

extern "C" ui_t* uiGet(wm_window_t* window) {
	return active_ui && active_ui->window == window ? active_ui : nullptr;
}

extern "C" bool uiConsumeBallSpawn(ui_t* ui, ui_ball_info_t* info) {
	if (!ui || !info) return false;
	mutexLock(ui->mutex);
	bool pending = ui->spawn_pending;
	if (pending) {
		*info = ui->ball_info;
		ui->spawn_pending = false;
	}
	mutexUnlock(ui->mutex);
	return pending;
}

extern "C" bool uiWantsMouse(ui_t* ui) {
	if (!ui) return false;
	mutexLock(ui->mutex);
	bool wants_mouse = false;
	if (ui->initialized) {
		ImGui::SetCurrentContext(ui->context);
		wants_mouse = ImGui::GetIO().WantCaptureMouse;
	}
	mutexUnlock(ui->mutex);
	return wants_mouse;
}

extern "C" bool uiWindowMessage(void* user_data, void* native_window,
	uint32_t message, uintptr_t w_param, intptr_t l_param) {
	ui_t* ui = (ui_t*)user_data;
	if (!ui) return false;
	mutexLock(ui->mutex);
	if (!ui->initialized) {
		mutexUnlock(ui->mutex);
		return false;
	}
	ImGui::SetCurrentContext(ui->context);
	LRESULT result = ImGui_ImplWin32_WndProcHandler((HWND)native_window,
		(UINT)message, (WPARAM)w_param, (LPARAM)l_param);
	ImGuiIO& io = ImGui::GetIO();
	bool mouse_message = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
	bool keyboard_message = (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
		message == WM_CHAR;
	bool handled = result != 0 || (mouse_message && io.WantCaptureMouse) ||
		(keyboard_message && io.WantCaptureKeyboard);
	mutexUnlock(ui->mutex);
	return handled;
}

extern "C" bool uiRendererInitialize(ui_t* ui, gpu_t* gpu) {
	if (!ui || !gpu) return false;
	mutexLock(ui->mutex);
	IMGUI_CHECKVERSION();
	ui->context = ImGui::CreateContext();
	ImGui::SetCurrentContext(ui->context);
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding = 6.0f;
	ImGui::GetStyle().FrameRounding = 4.0f;

	bool win32_ready = ImGui_ImplWin32_Init(wmGetHWND(ui->window));
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.ApiVersion = VK_API_VERSION_1_2;
	init_info.Instance = gpuGetInstance(gpu);
	init_info.PhysicalDevice = gpuGetPhysicalDevice(gpu);
	init_info.Device = gpuGetDevice(gpu);
	init_info.QueueFamily = gpuGetQueueFamilyIndex(gpu);
	init_info.Queue = gpuGetQueue(gpu);
	init_info.DescriptorPoolSize = 32;
	init_info.MinImageCount = gpuGetMinimumImageCount(gpu) < 2 ? 2 :
		gpuGetMinimumImageCount(gpu);
	init_info.ImageCount = gpuGetSwapchainImageCount(gpu);
	init_info.PipelineInfoMain.RenderPass = gpuGetRenderPass(gpu);
	init_info.PipelineInfoMain.Subpass = 0;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.CheckVkResultFn = uiCheckVulkanResult;
	bool vulkan_ready = win32_ready && ImGui_ImplVulkan_Init(&init_info);
	ui->initialized = vulkan_ready;
	mutexUnlock(ui->mutex);
	return vulkan_ready;
}

extern "C" void uiRendererDraw(ui_t* ui, gpu_t* gpu,
	gpu_cmd_buff_t* command_buffer) {
	if (!ui || !gpu || !command_buffer) return;
	mutexLock(ui->mutex);
	if (!ui->initialized) {
		mutexUnlock(ui->mutex);
		return;
	}
	ImGui::SetCurrentContext(ui->context);
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(440.0f, 520.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("XPBD Simulation");
	ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
	ImGui::SeparatorText("Spawn Ball");
	ImGui::InputFloat3("Position", ui->ball_info.position);
	ImGui::ColorEdit3("Color", ui->ball_info.color);
	ImGui::DragFloat("Radius", &ui->ball_info.radius, 0.01f, 0.1f, 5.0f, "%.2f");
	ImGui::DragFloat("Mass", &ui->ball_info.mass, 0.05f, 0.01f, 100.0f, "%.2f kg");
	ImGui::InputFloat("Structural compliance", &ui->ball_info.structural_compliance,
		1.0e-6f, 1.0e-5f, "%.3e");
	ImGui::InputFloat("Bending compliance", &ui->ball_info.bending_compliance,
		1.0e-6f, 1.0e-5f, "%.3e");
	ImGui::InputFloat("Volume compliance", &ui->ball_info.volume_compliance,
		1.0e-9f, 1.0e-8f, "%.3e");
	ImGui::SliderFloat("Velocity retention", &ui->ball_info.velocity_retention,
		0.0f, 1.0f, "%.2f");
	if (ImGui::Button("Spawn ball", ImVec2(-1.0f, 0.0f))) {
		ui->ball_info.radius = ui->ball_info.radius < 0.1f ? 0.1f : ui->ball_info.radius;
		ui->ball_info.mass = ui->ball_info.mass < 0.01f ? 0.01f : ui->ball_info.mass;
		ui->ball_info.structural_compliance = ui->ball_info.structural_compliance < 0.0f ?
			0.0f : ui->ball_info.structural_compliance;
		ui->ball_info.bending_compliance = ui->ball_info.bending_compliance < 0.0f ?
			0.0f : ui->ball_info.bending_compliance;
		ui->ball_info.volume_compliance = ui->ball_info.volume_compliance < 0.0f ?
			0.0f : ui->ball_info.volume_compliance;
		ui->spawn_pending = true;
	}
	if (ui->spawn_pending)
		ImGui::TextDisabled("Spawn queued for the scene thread...");
	ImGui::End();

	ImGui::Render();
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
		gpuGetCommandBuffer(command_buffer));
	mutexUnlock(ui->mutex);
}

extern "C" void uiRendererShutdown(ui_t* ui) {
	if (!ui) return;
	mutexLock(ui->mutex);
	if (ui->initialized) {
		ImGui::SetCurrentContext(ui->context);
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(ui->context);
		ui->context = nullptr;
		ui->initialized = false;
	}
	mutexUnlock(ui->mutex);
}
