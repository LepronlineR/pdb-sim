#include "wm.h"
#include "heap.h"
#include "fs.h"
#include "timer_object.h"
#include "timer.h"
#include "renderer.h"
#include "scene.h"
#include "ui.h"

#include "test.h"
#include "debug.h"

#include <stdint.h>
#include <string.h>

/* --smoke: drives the real app through an editor workflow with scripted input and
   checks each step: add objects to an empty scene, click to select, play, grab,
   delete and add during play, pause and step, stop (the scene must come back
   exactly as authored), delete in edit, load the demo, and play/stop again.
   Exits with the number of failed checks. */
typedef struct smoke_test_t {
	int frame;
	int failures;
	int max_grabbed;
	uint64_t authored_hash;
	uint64_t demo_hash;
	uint64_t moved_hash;
	int moved_id;
} smoke_test_t;

static void smokeCheck(smoke_test_t* smoke, bool passed, const char* what) {
	debugPrintConsole("[ %s ] frame %3d: %s\n", passed ? "PASS" : "FAIL", smoke->frame, what);
	if (!passed) smoke->failures++;
}

static void smokeCommand(ui_t* ui, ui_command_type_t type, int id) {
	ui_command_t command;
	memset(&command, 0, sizeof(command));
	command.type = type;
	command.id = id;
	uiPushCommand(ui, &command);
}

static bool smokeTestStep(smoke_test_t* smoke, scene_t* scene, ui_t* ui, wm_window_t* window) {
	int x, y, width, height;
	if (!wmGetCursorClientPos(window, &x, &y, &width, &height)) {
		width = 1280;
		height = 720;
	}
	scene_pointer_t center = { .x = width / 2, .y = height / 2, .width = width, .height = height };
	const int f = smoke->frame;
	switch (f) {
		case 0:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 0 && sceneGetMode(scene) == UI_MODE_EDIT,
				"scene starts empty in edit mode");
			smokeCommand(ui, UI_COMMAND_ADD_BALL, -1);   // id 1, at the camera target
			smokeCommand(ui, UI_COMMAND_ADD_CLOTH, -1);  // id 2, above it
			break;
		case 10:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 2, "ball and cloth added");
			smoke->authored_hash = sceneHashParticles(scene);
			smokeCommand(ui, UI_COMMAND_SELECT, -1);
			break;
		case 20:
			center.select_held = true;
			sceneSetPointerOverride(scene, &center);
			break;
		case 21:
			sceneSetPointerOverride(scene, &center);  // released: a click
			break;
		case 25:
			sceneSetPointerOverride(scene, NULL);
			smokeCheck(smoke, sceneGetSelectedId(scene) == 1,
				"click at the screen center selects the ball");
			break;
		case 30:
			smokeCommand(ui, UI_COMMAND_PLAY, -1);
			break;
		case 92:
			sceneSetPointerOverride(scene, &center);  // grab released
			smokeCheck(smoke, smoke->max_grabbed > 0, "right-drag grabs the ball while playing");
			break;
		case 95:
			sceneSetPointerOverride(scene, NULL);
			smokeCheck(smoke, sceneHashParticles(scene) != smoke->authored_hash,
				"playing moves the scene");
			smokeCommand(ui, UI_COMMAND_DELETE, 2);
			break;
		case 100:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 1, "deleting during play hides the cloth");
			smokeCommand(ui, UI_COMMAND_ADD_BALL, -1);
			break;
		case 105:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 2, "adding during play works");
			smokeCommand(ui, UI_COMMAND_PAUSE, -1);
			smokeCommand(ui, UI_COMMAND_STEP, -1);
			break;
		case 110:
			smokeCheck(smoke, sceneGetMode(scene) == UI_MODE_PAUSED, "pause and step");
			smokeCommand(ui, UI_COMMAND_STOP, -1);
			break;
		case 115:
			smokeCheck(smoke, sceneGetMode(scene) == UI_MODE_EDIT && sceneGetObjectCount(scene) == 2,
				"stop returns to edit with the authored objects");
			smokeCheck(smoke, sceneHashParticles(scene) == smoke->authored_hash,
				"stop restores every particle exactly");
			smokeCommand(ui, UI_COMMAND_SELECT, 1);
			smokeCommand(ui, UI_COMMAND_DELETE, 1);
			break;
		case 120:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 1 && sceneGetSelectedId(scene) == -1,
				"deleting in edit removes the object and the selection");
			smokeCommand(ui, UI_COMMAND_LOAD_DEMO, -1);
			break;
		case 125:
			smokeCheck(smoke, sceneGetObjectCount(scene) == 6, "demo scene loads");
			smoke->demo_hash = sceneHashParticles(scene);
			smokeCommand(ui, UI_COMMAND_PLAY, -1);
			break;
		case 245:
			smokeCommand(ui, UI_COMMAND_STOP, -1);
			break;
		case 250:
			smokeCheck(smoke, sceneHashParticles(scene) == smoke->demo_hash &&
				sceneGetObjectCount(scene) == 6, "the demo is restored after two seconds of play");
			break;
		case 272:
			sceneSetPointerOverride(scene, NULL);
			smokeCheck(smoke, sceneGetSelectedId(scene) > 0 && sceneHashParticles(scene) != smoke->demo_hash &&
				sceneGetObjectCount(scene) == 6, "right-drag in edit moves the object under the cursor");
			smoke->moved_hash = sceneHashParticles(scene);
			smoke->moved_id = sceneGetSelectedId(scene);
			smokeCommand(ui, UI_COMMAND_PLAY, -1);
			break;
		case 300:
			smokeCommand(ui, UI_COMMAND_STOP, -1);
			break;
		case 305: {
			smokeCheck(smoke, sceneHashParticles(scene) == smoke->moved_hash,
				"stop restores the moved scene exactly");
			ui_command_t command;
			memset(&command, 0, sizeof(command));
			command.type = UI_COMMAND_EDIT;
			sceneGetObjectProperties(scene, smoke->moved_id, &command.properties);
			command.properties.size += 0.5f;
			command.properties.mass = 2.0f;
			command.id = smoke->moved_id;
			uiPushCommand(ui, &command);
			break;
		}
		case 310: {
			ui_object_properties_t properties;
			const bool found = sceneGetObjectProperties(scene, smoke->moved_id, &properties);
			smokeCheck(smoke, found && properties.mass == 2.0f && sceneGetObjectCount(scene) == 6 &&
				sceneHashParticles(scene) != smoke->moved_hash, "inspector edits rebuild the object");
			break;
		}
	}
	/* Edit-mode move: press on the object at the center, drag the pointer up,
	   release. */
	if (f >= 255 && f < 270) {
		scene_pointer_t drag = center;
		drag.y = center.y - (f - 255) * 4;
		drag.grab_held = f < 269;
		sceneSetPointerOverride(scene, &drag);
	}
	if (f > 30 && f < 92) {
		center.grab_held = true;
		sceneSetPointerOverride(scene, &center);
		const int grabbed = sceneGetGrabbedCount(scene);
		smoke->max_grabbed = grabbed > smoke->max_grabbed ? grabbed : smoke->max_grabbed;
	}
	smoke->frame++;
	return f < 315;
}

int main(int argc, const char*argv[]) {

	debugInstallExceptionHandler();
	debugSetPrintMask(DEBUG_PRINT_INFO | DEBUG_PRINT_WARNING | DEBUG_PRINT_ERROR);

	timerStartup();

	heap_t* heap = heapCreate(256 * 1024 * 1024); // 256 MBs
	bool smoke_mode = false;
	bool demo_mode = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--test") == 0) {
			const int failures = testPhysics(heap);
			heapDestroy(heap);
			return failures;
		}
		smoke_mode = smoke_mode || strcmp(argv[i], "--smoke") == 0;
		demo_mode = demo_mode || strcmp(argv[i], "--demo") == 0;
	}
	wm_window_t* window = wmCreateWindow(heap);
	ui_t* ui = uiCreate(heap, window);
	fs_t* fs = fsCreate(heap, 8);
	timer_object_t* root_time = timerObjectCreate(heap, NULL);
	renderer_t* renderer = rendererCreate(heap, window);

	scene_t* scene = sceneCreate(heap, fs, window, renderer);
	if (demo_mode) {
		smokeCommand(ui, UI_COMMAND_LOAD_DEMO, -1);
	}

	smoke_test_t smoke = { 0 };
	while (wmPumpWindow(window)) {
		if (smoke_mode && !smokeTestStep(&smoke, scene, ui, window)) break;
		// update scene
		sceneUpdate(scene);
	}

	rendererDestroy(renderer);
	uiDestroy(ui);
	sceneDestroy(scene);
	timerObjectDestroy(root_time);
	fsDestroy(fs);
	wmDestroyWindow(window);
	heapDestroy(heap);
	if (smoke_mode) {
		const bool finished = smoke.frame > 310;
		if (!finished) smoke.failures++;
		debugPrintConsole("smoke test %s: %d frames, %d failed check(s)\n",
			smoke.failures == 0 ? "passed" : "FAILED", smoke.frame, smoke.failures);
		return smoke.failures;
	}
	return 0;
}
