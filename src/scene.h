#ifndef __SCENE_H__
#define __SCENE_H__

#include "ball.h"
#include "cloth.h"
#include "ecs.h"
#include "ui.h"

/*		SCENE
*
*	Works like a game-engine editor scene. The scene holds authored objects (balls
*	and cloths with their settings) and is in one of three modes:
*
*	  Edit     nothing simulates; adding, moving, editing and deleting objects
*	           changes the authored scene
*	  Playing  the physics world steps; objects added or deleted now are only
*	           runtime changes
*	  Paused   playing, but frozen (Step advances one frame)
*
*	Stop returns to Edit and rebuilds the world from the authored objects, so the
*	scene is exactly as it was before Play. The physics world is append-only, so
*	every structural change (delete, edit, stop) rebuilds it from the objects.
*/

typedef struct scene_t scene_t;

// Pointer state the scene reads for picking. Normally it comes from the window;
// a scripted test can supply its own.
typedef struct scene_pointer_t {
	int x;
	int y;
	int width;
	int height;
	bool select_held;  // left button: a press and release without dragging selects
	bool grab_held;    // right button: move (edit) or grab (play)
} scene_pointer_t;

typedef struct fs_t fs_t;
typedef struct heap_t heap_t;
typedef struct renderer_t renderer_t;
typedef struct wm_window_t wm_window_t;
typedef struct physics_t physics_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render);

void sceneDestroy(scene_t* scene);

void sceneUpdate(scene_t* scene);
void sceneLoadResources(scene_t* scene);
void sceneUnloadResources(scene_t* scene);
void sceneSpawnCamera(scene_t* scene);
ecs_entity_t sceneSpawnModel(scene_t* scene, const char* name,
	vec3f_t position, vec3f_t scale, gpu_mesh_info_t* mesh);
void sceneUpdateCamera(scene_t* scene);
void sceneUpdateInteraction(scene_t* scene);
void sceneUpdateSimulation(scene_t* scene);
void sceneDrawModels(scene_t* scene);

// Replaces window input for picking while set; NULL returns to the window.
void sceneSetPointerOverride(scene_t* scene, const scene_pointer_t* pointer);

// Queries for scripted tests.
//
// Number of objects present in the current mode (hidden ones excluded).
int sceneGetObjectCount(scene_t* scene);
// Id of the selected object, or -1.
int sceneGetSelectedId(scene_t* scene);
// ui_play_mode_t of the scene.
int sceneGetMode(scene_t* scene);
// Number of particles currently held by the pointer.
int sceneGetGrabbedCount(scene_t* scene);
// Copies an object's editable settings, as the inspector sees them.
//
// RETURN: false if no object has that id
// Number of textures that loaded (see texture_library.h).
int sceneGetTextureCount(scene_t* scene);
// Whether the sky panorama is drawn this frame.
bool sceneIsSkyVisible(scene_t* scene);
bool sceneGetObjectProperties(scene_t* scene, int id, ui_object_properties_t* properties);
// Client-pixel position of an object's authored position.
//
// RETURN: false if there is no such object or it is behind the camera
bool sceneGetObjectScreenPosition(scene_t* scene, int id, int width, int height, int* x, int* y);
// Client-pixel position of a move-gizmo handle (axis 0 = X, 1 = Y, 2 = Z) on the
// selection, for a window of the given size.
//
// RETURN: false if the gizmo is not showing
bool sceneGetGizmoHandle(scene_t* scene, int axis, int width, int height, int* x, int* y);
// A hash of every particle position, to check that Stop restores the scene.
uint64_t sceneHashParticles(scene_t* scene);

#endif
