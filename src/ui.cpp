#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
#include "gpu.h"
#include "heap.h"
#include "mutex.h"
#include "ui.h"
#include "wm.h"
}

#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND window, UINT message, WPARAM w_param, LPARAM l_param);

enum {
	UI_COMMAND_CAPACITY = 64
};

struct ui_t {
	heap_t* heap;
	wm_window_t* window;
	mutex_t* mutex;
	ImGuiContext* context;
	bool initialized;

	// Commands for the scene, a ring buffer drained by uiPopCommand.
	ui_command_t commands[UI_COMMAND_CAPACITY];
	int command_head;
	int command_count;

	ui_scene_view_t view;
	ui_sim_settings_t settings;

	// The inspector edits a copy of the selected object's properties, reloaded
	// when the selection or the scene-side revision changes.
	ui_object_properties_t inspector;
	int inspector_id;
	uint32_t inspector_revision;
};

static ui_t* active_ui;

extern "C" void uiCheckVulkanResult(VkResult result) {
	(void)result;
}

extern "C" ui_t* uiCreate(heap_t* heap, wm_window_t* window) {
	ui_t* ui = (ui_t*)heapAlloc(heap, sizeof(ui_t), 8);
	memset(ui, 0, sizeof(*ui));
	ui->heap = heap;
	ui->window = window;
	ui->mutex = mutexCreate();
	ui->context = nullptr;
	ui->view.mode = UI_MODE_EDIT;
	ui->view.selected_id = -1;
	ui->inspector_id = -1;
	ui->settings.substeps = 8;
	ui->settings.iterations = 1;
	ui->settings.gravity = -9.81f;
	ui->settings.static_friction = 0.6f;
	ui->settings.dynamic_friction = 0.4f;
	ui->settings.restitution = 0.2f;
	ui->settings.rolling_resistance = 0.15f;
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

/* Callers hold the mutex. A full queue drops the command; at one or two
   commands per frame that only happens if the scene stops draining it. */
static void uiQueueCommand(ui_t* ui, const ui_command_t* command) {
	if (ui->command_count >= UI_COMMAND_CAPACITY) return;
	const int tail = (ui->command_head + ui->command_count) % UI_COMMAND_CAPACITY;
	ui->commands[tail] = *command;
	ui->command_count++;
}

static void uiQueueSimple(ui_t* ui, ui_command_type_t type, int id) {
	ui_command_t command;
	memset(&command, 0, sizeof(command));
	command.type = type;
	command.id = id;
	uiQueueCommand(ui, &command);
}

extern "C" void uiPushCommand(ui_t* ui, const ui_command_t* command) {
	if (!ui || !command) return;
	mutexLock(ui->mutex);
	uiQueueCommand(ui, command);
	mutexUnlock(ui->mutex);
}

extern "C" bool uiPopCommand(ui_t* ui, ui_command_t* command) {
	if (!ui || !command) return false;
	mutexLock(ui->mutex);
	const bool available = ui->command_count > 0;
	if (available) {
		*command = ui->commands[ui->command_head];
		ui->command_head = (ui->command_head + 1) % UI_COMMAND_CAPACITY;
		ui->command_count--;
	}
	mutexUnlock(ui->mutex);
	return available;
}

extern "C" void uiSetSceneView(ui_t* ui, const ui_scene_view_t* view) {
	if (!ui || !view) return;
	mutexLock(ui->mutex);
	ui->view = *view;
	mutexUnlock(ui->mutex);
}

extern "C" ui_sim_settings_t uiGetSimSettings(ui_t* ui) {
	if (!ui) {
		return ui_sim_settings_t{ 8, 1, -9.81f, 0.6f, 0.4f, 0.2f, 0.15f };
	}
	mutexLock(ui->mutex);
	ui_sim_settings_t settings = ui->settings;
	mutexUnlock(ui->mutex);
	return settings;
}

static void uiDrawToolbar(ui_t* ui) {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, 12.0f), ImGuiCond_FirstUseEver,
		ImVec2(0.5f, 0.0f));
	ImGui::Begin("Simulation", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
	const ui_play_mode_t mode = ui->view.mode;
	if (mode == UI_MODE_EDIT) {
		if (ImGui::Button("  Play  ")) uiQueueSimple(ui, UI_COMMAND_PLAY, -1);
	}
	else {
		if (ImGui::Button("  Stop  ")) uiQueueSimple(ui, UI_COMMAND_STOP, -1);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(mode == UI_MODE_EDIT);
	if (mode == UI_MODE_PAUSED) {
		if (ImGui::Button("Resume")) uiQueueSimple(ui, UI_COMMAND_RESUME, -1);
	}
	else if (ImGui::Button("Pause ")) {
		uiQueueSimple(ui, UI_COMMAND_PAUSE, -1);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(mode != UI_MODE_PAUSED);
	if (ImGui::Button("Step")) uiQueueSimple(ui, UI_COMMAND_STEP, -1);
	ImGui::EndDisabled();
	ImGui::SameLine();
	static const char* mode_names[] = { "EDIT", "PLAYING", "PAUSED" };
	static const ImVec4 mode_colors[] = {
		ImVec4(0.6f, 0.8f, 1.0f, 1.0f), ImVec4(0.4f, 1.0f, 0.5f, 1.0f), ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
	};
	ImGui::TextColored(mode_colors[mode], "%s", mode_names[mode]);

	const ui_stats_t* stats = &ui->view.stats;
	ImGui::Text("FPS %.0f   physics %.2f ms   %d particles   %d contacts",
		ImGui::GetIO().Framerate, stats->physics_ms, stats->particle_count, stats->contact_count);
	if (stats->dropped_ms > 0.0f) {
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
			"Physics fell behind: %.0f ms of simulation dropped", stats->dropped_ms);
	}
	ImGui::TextDisabled(mode == UI_MODE_EDIT ?
		"Click: select   Right-drag: move   Del: delete   Left-drag: orbit   Wheel: zoom" :
		"Right-drag: grab   Left-drag: orbit   Wheel: zoom   Stop restores the scene");

	if (ImGui::CollapsingHeader("Solver")) {
		ImGui::PushItemWidth(180.0f);
		ImGui::SliderInt("Substeps", &ui->settings.substeps, 1, 32);
		ImGui::SliderInt("Iterations", &ui->settings.iterations, 1, 8);
		ImGui::SliderFloat("Gravity", &ui->settings.gravity, -30.0f, 0.0f, "%.2f m/s^2");
		ImGui::SliderFloat("Static friction", &ui->settings.static_friction, 0.0f, 1.5f, "%.2f");
		ImGui::SliderFloat("Dynamic friction", &ui->settings.dynamic_friction, 0.0f, 1.5f, "%.2f");
		ImGui::SliderFloat("Restitution", &ui->settings.restitution, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Rolling resistance", &ui->settings.rolling_resistance, 0.0f, 0.5f, "%.2f");
		ImGui::PopItemWidth();
	}
	ImGui::End();
}

static void uiDrawHierarchy(ui_t* ui) {
	ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(330.0f, 380.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("Hierarchy");
	const bool editing = ui->view.mode == UI_MODE_EDIT;
	if (ImGui::Button("Add Ball")) uiQueueSimple(ui, UI_COMMAND_ADD_BALL, -1);
	ImGui::SameLine();
	if (ImGui::Button("Add Cloth")) uiQueueSimple(ui, UI_COMMAND_ADD_CLOTH, -1);
	ImGui::SameLine();
	if (ImGui::Button("Add Sphere")) uiQueueSimple(ui, UI_COMMAND_ADD_SPHERE, -1);
	ImGui::BeginDisabled(!editing);
	if (ImGui::Button("Trampoline demo")) uiQueueSimple(ui, UI_COMMAND_LOAD_DEMO, -1);
	ImGui::SameLine();
	if (ImGui::Button("Drape demo")) uiQueueSimple(ui, UI_COMMAND_LOAD_DRAPE_DEMO, -1);
	ImGui::SameLine();
	if (ImGui::Button("Clear")) uiQueueSimple(ui, UI_COMMAND_CLEAR, -1);
	ImGui::EndDisabled();
	ImGui::Separator();
	if (ui->view.object_count == 0) {
		ImGui::TextDisabled("The scene is empty. Add an object or load a demo.");
	}
	for (int i = 0; i < ui->view.object_count; ++i) {
		const ui_object_t* object = &ui->view.objects[i];
		char label[UI_NAME_LENGTH + 32];
		snprintf(label, sizeof(label), "%s%s##%d", object->name,
			object->transient ? "  (play)" : "", object->id);
		if (ImGui::Selectable(label, object->id == ui->view.selected_id)) {
			uiQueueSimple(ui, UI_COMMAND_SELECT, object->id);
		}
	}
	ImGui::End();
}

static bool uiPinCombo(int* pin) {
	static const char* pin_names[] = { "None (falls)", "Four corners", "One edge", "Whole border" };
	return ImGui::Combo("Pinned", pin, pin_names, IM_ARRAYSIZE(pin_names));
}

static void uiDrawInspector(ui_t* ui) {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImGui::SetNextWindowPos(ImVec2(display.x - 12.0f, 12.0f), ImGuiCond_FirstUseEver,
		ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(330.0f, 430.0f), ImGuiCond_FirstUseEver);
	ImGui::Begin("Inspector");
	const int selected = ui->view.selected_id;
	if (selected < 0) {
		ui->inspector_id = -1;
		ImGui::TextDisabled("Nothing selected.");
		ImGui::End();
		return;
	}
	/* Reload unless the user is in the middle of editing a field, so scene-side
	   changes (a drag in the viewport) show up without fighting the user. */
	const bool reload = ui->inspector_id != selected ||
		ui->inspector_revision != ui->view.selected_revision;
	if (reload && !ImGui::IsAnyItemActive()) {
		ui->inspector = ui->view.selected;
		ui->inspector_id = selected;
		ui->inspector_revision = ui->view.selected_revision;
	}

	ui_object_properties_t* p = &ui->inspector;
	const bool editing = ui->view.mode == UI_MODE_EDIT;
	ImGui::Text("%s", p->name);
	ImGui::SameLine();
	ImGui::TextDisabled(p->kind == UI_OBJECT_BALL ? "(ball)" :
		p->kind == UI_OBJECT_CLOTH ? "(cloth)" : "(solid sphere)");
	if (!editing) {
		ImGui::TextDisabled("Read-only while playing: Stop to edit.");
	}
	ImGui::BeginDisabled(!editing);
	ImGui::PushItemWidth(170.0f);
	bool changed = false;
	changed |= ImGui::DragFloat3("Position", p->position, 0.02f);
	changed |= ImGui::ColorEdit3("Color", p->color);
	if (p->kind == UI_OBJECT_SPHERE) {
		changed |= ImGui::DragFloat("Radius", &p->radius, 0.01f, 0.1f, 10.0f, "%.2f m");
		ImGui::TextDisabled("Static: it never moves while playing.");
	}
	else {
		changed |= ImGui::DragFloat("Mass", &p->mass, 0.02f, 0.01f, 100.0f, "%.2f kg");
	}
	if (p->kind == UI_OBJECT_BALL) {
		changed |= ImGui::DragFloat("Radius", &p->radius, 0.01f, 0.1f, 5.0f, "%.2f m");
		changed |= ImGui::InputFloat("Structural compl.", &p->stretch_compliance, 1.0e-4f, 1.0e-3f, "%.2e");
		changed |= ImGui::InputFloat("Bending compl.", &p->bending_compliance, 1.0e-4f, 1.0e-3f, "%.2e");
		changed |= ImGui::InputFloat("Volume compl.", &p->volume_compliance, 1.0e-9f, 1.0e-8f, "%.2e");
	}
	else if (p->kind == UI_OBJECT_CLOTH) {
		changed |= ImGui::SliderInt("Resolution", &p->resolution, 4, 64);
		changed |= ImGui::DragFloat("Size", &p->size, 0.02f, 0.5f, 10.0f, "%.2f m");
		changed |= ImGui::InputFloat("Stretch compl.", &p->stretch_compliance, 1.0e-4f, 1.0e-3f, "%.2e");
		changed |= ImGui::InputFloat("Shear compl.", &p->shear_compliance, 1.0e-4f, 1.0e-3f, "%.2e");
		changed |= ImGui::InputFloat("Bend compl.", &p->bending_compliance, 1.0e-3f, 1.0e-2f, "%.2e");
		changed |= ImGui::SliderFloat("Air drag", &p->drag, 0.0f, 3.0f, "%.2f /s");
		changed |= uiPinCombo(&p->pin);
	}
	if (p->kind != UI_OBJECT_SPHERE) {
		changed |= ImGui::SliderFloat("Velocity retention", &p->velocity_retention, 0.0f, 1.0f, "%.2f");
	}
	ImGui::PopItemWidth();
	ImGui::EndDisabled();
	if (changed && editing) {
		ui_command_t command;
		memset(&command, 0, sizeof(command));
		command.type = UI_COMMAND_EDIT;
		command.id = p->id;
		command.properties = *p;
		uiQueueCommand(ui, &command);
	}
	ImGui::Separator();
	if (ImGui::Button("Delete", ImVec2(-1.0f, 0.0f))) uiQueueSimple(ui, UI_COMMAND_DELETE, selected);
	ImGui::End();
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

	uiDrawToolbar(ui);
	uiDrawHierarchy(ui);
	uiDrawInspector(ui);
	/* Delete deletes the selection unless a text field has the keyboard. */
	if (ui->view.selected_id >= 0 && !ImGui::GetIO().WantTextInput &&
		ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
		uiQueueSimple(ui, UI_COMMAND_DELETE, ui->view.selected_id);
	}

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
