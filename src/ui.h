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

#ifdef __cplusplus
extern "C" {
#endif

ui_t* uiCreate(heap_t* heap, wm_window_t* window);
void uiDestroy(ui_t* ui);
ui_t* uiGet(wm_window_t* window);
bool uiConsumeBallSpawn(ui_t* ui, ui_ball_info_t* info);
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
