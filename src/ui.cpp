#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
#include "gpu.h"
#include "heap.h"
#include "mutex.h"
#include "ui.h"
#include "wm.h"
}

#include <string.h>

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
	ui_cloth_info_t cloth_info;
	ui_sim_settings_t settings;
	ui_stats_t stats;
	bool initialized;
	bool spawn_pending;
	bool cloth_spawn_pending;
	bool reset_pending;
	bool step_pending;
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
	ui->ball_info.structural_compliance = 3.0e-3f;
	ui->ball_info.bending_compliance = 3.0e-3f;
	ui->ball_info.volume_compliance = 1.0e-8f;
	ui->ball_info.velocity_retention = 0.30f;
	ui->cloth_info.position[0] = 0.0f;
	ui->cloth_info.position[1] = 6.0f;
	ui->cloth_info.position[2] = 0.0f;
	ui->cloth_info.color[0] = 0.92f;
	ui->cloth_info.color[1] = 0.86f;
	ui->cloth_info.color[2] = 0.70f;
	ui->cloth_info.resolution = 32;
	ui->cloth_info.size = 4.0f;
	ui->cloth_info.mass = 1.0f;
	ui->cloth_info.stretch_compliance = 1.0e-7f;
	ui->cloth_info.bend_compliance = 5.0e-2f;
	ui->cloth_info.velocity_retention = 0.6f;
	ui->cloth_info.pin = 0;
	ui->settings.substeps = 8;
	ui->settings.iterations = 1;
	ui->settings.gravity = -9.81f;
	ui->settings.static_friction = 0.6f;
	ui->settings.dynamic_friction = 0.4f;
	ui->settings.restitution = 0.2f;
	ui->settings.rolling_resistance = 0.15f;
	ui->settings.paused = false;
	memset(&ui->stats, 0, sizeof(ui->stats));
	ui->initialized = false;
	ui->spawn_pending = false;
	ui->cloth_spawn_pending = false;
	ui->reset_pending = false;
	ui->step_pending = false;
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

extern "C" bool uiConsumeClothSpawn(ui_t* ui, ui_cloth_info_t* info) {
	if (!ui || !info) return false;
	mutexLock(ui->mutex);
	bool pending = ui->cloth_spawn_pending;
	if (pending) {
		*info = ui->cloth_info;
		ui->cloth_spawn_pending = false;
	}
	mutexUnlock(ui->mutex);
	return pending;
}

extern "C" bool uiConsumeReset(ui_t* ui) {
	if (!ui) return false;
	mutexLock(ui->mutex);
	bool pending = ui->reset_pending;
	ui->reset_pending = false;
	mutexUnlock(ui->mutex);
	return pending;
}

extern "C" bool uiConsumeStep(ui_t* ui) {
	if (!ui) return false;
	mutexLock(ui->mutex);
	bool pending = ui->step_pending;
	ui->step_pending = false;
	mutexUnlock(ui->mutex);
	return pending;
}

extern "C" void uiRequestReset(ui_t* ui) {
	if (!ui) return;
	mutexLock(ui->mutex);
	ui->reset_pending = true;
	mutexUnlock(ui->mutex);
}

extern "C" void uiRequestBallSpawn(ui_t* ui) {
	if (!ui) return;
	mutexLock(ui->mutex);
	ui->spawn_pending = true;
	mutexUnlock(ui->mutex);
}

extern "C" void uiRequestClothSpawn(ui_t* ui) {
	if (!ui) return;
	mutexLock(ui->mutex);
	ui->cloth_spawn_pending = true;
	mutexUnlock(ui->mutex);
}

extern "C" ui_sim_settings_t uiGetSimSettings(ui_t* ui) {
	if (!ui) {
		return ui_sim_settings_t{ 8, 1, -9.81f, 0.6f, 0.4f, 0.2f, 0.15f, false };
	}
	mutexLock(ui->mutex);
	ui_sim_settings_t settings = ui->settings;
	mutexUnlock(ui->mutex);
	return settings;
}

extern "C" void uiSetStats(ui_t* ui, const ui_stats_t* stats) {
	if (!ui || !stats) return;
	mutexLock(ui->mutex);
	ui->stats = *stats;
	mutexUnlock(ui->mutex);
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
	ImGui::SetNextWindowSize(ImVec2(440.0f, 700.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("XPBD Simulation");
	ImGui::Text("FPS: %.1f   physics: %.2f ms", ImGui::GetIO().Framerate,
		ui->stats.physics_ms);
	ImGui::Text("%d particles, %d constraints, %d bodies, %d contacts",
		ui->stats.particle_count, ui->stats.constraint_count, ui->stats.soft_body_count,
		ui->stats.contact_count);
	if (ui->stats.dropped_ms > 0.0f)
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
			"Physics fell behind: %.0f ms of simulation dropped", ui->stats.dropped_ms);
	ImGui::TextDisabled("Left-drag: orbit   Right-drag: grab   Wheel: zoom");

	ImGui::SeparatorText("Simulation");
	ImGui::Checkbox("Paused", &ui->settings.paused);
	ImGui::SameLine();
	ImGui::BeginDisabled(!ui->settings.paused);
	if (ImGui::Button("Step")) ui->step_pending = true;
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Reset scene")) ui->reset_pending = true;
	ImGui::SliderInt("Substeps", &ui->settings.substeps, 1, 32);
	ImGui::SliderInt("Iterations", &ui->settings.iterations, 1, 8);
	ImGui::SliderFloat("Gravity", &ui->settings.gravity, -30.0f, 0.0f, "%.2f m/s^2");
	ImGui::SliderFloat("Static friction", &ui->settings.static_friction, 0.0f, 1.5f, "%.2f");
	ImGui::SliderFloat("Dynamic friction", &ui->settings.dynamic_friction, 0.0f, 1.5f, "%.2f");
	ImGui::SliderFloat("Restitution", &ui->settings.restitution, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Rolling resistance", &ui->settings.rolling_resistance, 0.0f, 0.5f, "%.2f");

	ImGui::SeparatorText("Spawn Ball");
	ImGui::InputFloat3("Position", ui->ball_info.position);
	ImGui::ColorEdit3("Color", ui->ball_info.color);
	ImGui::DragFloat("Radius", &ui->ball_info.radius, 0.01f, 0.1f, 5.0f, "%.2f");
	ImGui::DragFloat("Mass", &ui->ball_info.mass, 0.05f, 0.01f, 100.0f, "%.2f kg");
	ImGui::InputFloat("Structural compliance", &ui->ball_info.structural_compliance,
		1.0e-4f, 1.0e-3f, "%.3e");
	ImGui::InputFloat("Bending compliance", &ui->ball_info.bending_compliance,
		1.0e-4f, 1.0e-3f, "%.3e");
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

	ImGui::SeparatorText("Spawn Cloth");
	ImGui::PushID("cloth");
	ImGui::InputFloat3("Position", ui->cloth_info.position);
	ImGui::ColorEdit3("Color", ui->cloth_info.color);
	ImGui::SliderInt("Resolution", &ui->cloth_info.resolution, 4, 64);
	ImGui::DragFloat("Size", &ui->cloth_info.size, 0.05f, 0.5f, 10.0f, "%.2f m");
	ImGui::DragFloat("Mass", &ui->cloth_info.mass, 0.05f, 0.01f, 50.0f, "%.2f kg");
	ImGui::InputFloat("Stretch compliance", &ui->cloth_info.stretch_compliance,
		1.0e-8f, 1.0e-7f, "%.3e");
	ImGui::InputFloat("Bend compliance", &ui->cloth_info.bend_compliance,
		1.0e-4f, 1.0e-3f, "%.3e");
	ImGui::SliderFloat("Velocity retention", &ui->cloth_info.velocity_retention,
		0.0f, 1.0f, "%.2f");
	static const char* pin_names[] = { "None (falls)", "Four corners", "One edge", "Whole border" };
	ImGui::Combo("Pinned", &ui->cloth_info.pin, pin_names, IM_ARRAYSIZE(pin_names));
	if (ImGui::Button("Spawn cloth", ImVec2(-1.0f, 0.0f))) {
		ui->cloth_info.size = ui->cloth_info.size < 0.5f ? 0.5f : ui->cloth_info.size;
		ui->cloth_info.mass = ui->cloth_info.mass < 0.01f ? 0.01f : ui->cloth_info.mass;
		ui->cloth_info.stretch_compliance = ui->cloth_info.stretch_compliance < 0.0f ?
			0.0f : ui->cloth_info.stretch_compliance;
		ui->cloth_info.bend_compliance = ui->cloth_info.bend_compliance < 0.0f ?
			0.0f : ui->cloth_info.bend_compliance;
		ui->cloth_spawn_pending = true;
	}
	ImGui::PopID();
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
