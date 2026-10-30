#ifndef __UI_H__
#define __UI_H__

#include "gpu.h"
#include <stdbool.h>
#include <stdint.h>

/*		EDITOR UI
*
*	The UI runs on the render thread and never touches the scene directly. It sends
*	commands (play, add, select, delete, edit...) that the scene drains each frame,
*	and draws a snapshot of the scene that the scene publishes each frame.
*/

typedef struct ui_t ui_t;
typedef struct heap_t heap_t;
typedef struct wm_window_t wm_window_t;

enum {
	UI_MAX_OBJECTS = 32,
	UI_NAME_LENGTH = 32
};

typedef enum ui_play_mode_t {
	UI_MODE_EDIT,     // authoring: nothing simulates, changes are kept
	UI_MODE_PLAYING,  // simulating; Stop returns to the scene as it was before Play
	UI_MODE_PAUSED    // simulation frozen mid-play
} ui_play_mode_t;

// Edit-mode tools.
typedef enum ui_tool_t {
	UI_TOOL_MOVE,  // a move gizmo on the selection: drag a handle along X, Y or Z
	UI_TOOL_PIN    // clicking a cloth particle pins it in place (or unpins it)
} ui_tool_t;

typedef enum ui_object_kind_t {
	UI_OBJECT_BALL,
	UI_OBJECT_CLOTH,
	UI_OBJECT_SPHERE  // solid, static sphere collider
} ui_object_kind_t;

// Every editable setting of a scene object. Ball and cloth share the common
// fields; the rest apply to one kind only.
typedef struct ui_object_properties_t {
	int id;
	ui_object_kind_t kind;
	char name[UI_NAME_LENGTH];
	float position[3];
	float color[3];
	float mass;
	float velocity_retention;
	float stretch_compliance;  // ball: structural springs, cloth: stretch
	float bending_compliance;
	// ball and sphere
	float radius;
	// ball
	float volume_compliance;
	// cloth
	int resolution;
	float size;
	float shear_compliance;
	float drag;
	int pin;  // cloth_pin_t
	int pin_count;  // read-only: preset plus hand-placed pins
} ui_object_properties_t;

typedef struct ui_object_t {
	int id;
	ui_object_kind_t kind;
	char name[UI_NAME_LENGTH];
	bool transient;  // added during play; removed on Stop
} ui_object_t;

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

// What the scene publishes each frame.
typedef struct ui_scene_view_t {
	ui_play_mode_t mode;
	ui_tool_t tool;
	int object_count;
	ui_object_t objects[UI_MAX_OBJECTS];
	int selected_id;  // -1 when nothing is selected
	ui_object_properties_t selected;
	// Bumped whenever the selected object's properties change on the scene side
	// (selection changed, object moved), so the inspector reloads them.
	uint32_t selected_revision;
	ui_stats_t stats;
} ui_scene_view_t;

typedef enum ui_command_type_t {
	UI_COMMAND_PLAY,
	UI_COMMAND_STOP,
	UI_COMMAND_PAUSE,
	UI_COMMAND_RESUME,
	UI_COMMAND_STEP,
	UI_COMMAND_ADD_BALL,
	UI_COMMAND_ADD_CLOTH,
	UI_COMMAND_ADD_SPHERE,
	UI_COMMAND_LOAD_DEMO,
	UI_COMMAND_LOAD_DRAPE_DEMO,
	UI_COMMAND_CLEAR,
	UI_COMMAND_SELECT,      // id, or -1 to deselect
	UI_COMMAND_SET_TOOL,    // id is a ui_tool_t
	UI_COMMAND_CLEAR_PINS,  // id: remove a cloth's hand-placed pins
	UI_COMMAND_DELETE,      // id
	UI_COMMAND_EDIT         // properties.id gets properties
} ui_command_type_t;

typedef struct ui_command_t {
	ui_command_type_t type;
	int id;
	ui_object_properties_t properties;
} ui_command_t;

// Solver settings edited in the UI and applied by the scene every frame.
typedef struct ui_sim_settings_t {
	int substeps;
	int iterations;
	float gravity;
	float static_friction;
	float dynamic_friction;
	float restitution;
	float rolling_resistance;
} ui_sim_settings_t;

#ifdef __cplusplus
extern "C" {
#endif

ui_t* uiCreate(heap_t* heap, wm_window_t* window);
void uiDestroy(ui_t* ui);
ui_t* uiGet(wm_window_t* window);

// Queues a command for the scene; also used by scripted tests.
void uiPushCommand(ui_t* ui, const ui_command_t* command);
// RETURN: true and the oldest queued command, or false when the queue is empty
bool uiPopCommand(ui_t* ui, ui_command_t* command);
void uiSetSceneView(ui_t* ui, const ui_scene_view_t* view);
ui_sim_settings_t uiGetSimSettings(ui_t* ui);
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
