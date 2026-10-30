#ifndef __UI_H__
#define __UI_H__

#include "gpu.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ui_t ui_t;
typedef struct heap_t heap_t;
typedef struct wm_window_t wm_window_t;

typedef struct ui_ball_info_t {
	float position[3];
	float color[3];
	float radius;
	float mass;
	float structural_compliance;
	float bending_compliance;
	float volume_compliance;
	float velocity_retention;
} ui_ball_info_t;

typedef struct ui_cloth_info_t {
	float position[3];
	float color[3];
	int resolution;
	float size;
	float mass;
	float stretch_compliance;
	float bend_compliance;
	float velocity_retention;
	int pin; // cloth_pin_t
} ui_cloth_info_t;

// Solver settings edited in the UI and applied by the scene every frame.
typedef struct ui_sim_settings_t {
	int substeps;
	int iterations;
	float gravity;
	float static_friction;
	float dynamic_friction;
	float restitution;
	float rolling_resistance;
	bool paused;
} ui_sim_settings_t;

// Numbers the scene reports back for display.
typedef struct ui_stats_t {
	float physics_ms;
	float dropped_ms;
	int particle_count;
	int constraint_count;
	int soft_body_count;
	int contact_count;
	bool grabbing;
} ui_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

ui_t* uiCreate(heap_t* heap, wm_window_t* window);
void uiDestroy(ui_t* ui);
ui_t* uiGet(wm_window_t* window);
bool uiConsumeBallSpawn(ui_t* ui, ui_ball_info_t* info);
bool uiConsumeClothSpawn(ui_t* ui, ui_cloth_info_t* info);
bool uiConsumeReset(ui_t* ui);
// True once per press of "Step" while paused.
bool uiConsumeStep(ui_t* ui);
ui_sim_settings_t uiGetSimSettings(ui_t* ui);
// Queue the same actions the buttons do, for scripted tests.
void uiRequestReset(ui_t* ui);
void uiRequestBallSpawn(ui_t* ui);
void uiRequestClothSpawn(ui_t* ui);
void uiSetStats(ui_t* ui, const ui_stats_t* stats);
bool uiWantsMouse(ui_t* ui);

bool uiWindowMessage(void* user_data, void* native_window,
	uint32_t message, uintptr_t w_param, intptr_t l_param);
bool uiRendererInitialize(ui_t* ui, gpu_t* gpu);
void uiRendererDraw(ui_t* ui, gpu_t* gpu, gpu_cmd_buff_t* command_buffer);
void uiRendererShutdown(ui_t* ui);
void uiCheckVulkanResult(VkResult result);

#ifdef __cplusplus
}
#endif

#endif
