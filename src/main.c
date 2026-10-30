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

#include <string.h>

/* --smoke: drives the real app with scripted input (grab and drag at the screen
   center, release, reset the scene, spawn a ball and a cloth) and exits with 0 if
   every step happened. Exercises the paths a human would click through. */
typedef struct smoke_test_t {
	int frame;
	int max_grabbed;
	int resets;
} smoke_test_t;

static bool smokeTestStep(smoke_test_t* smoke, scene_t* scene, ui_t* ui, wm_window_t* window) {
	int x, y, width, height;
	if (!wmGetCursorClientPos(window, &x, &y, &width, &height)) {
		width = 1280;
		height = 720;
	}
	const int f = smoke->frame++;
	if (f >= 120 && f < 260) {
		const float t = (float)(f - 120) / 140.0f;
		scene_pointer_t pointer = {
			.x = width / 2, .y = height / 2 - (int)(t * (float)height * 0.25f),
			.width = width, .height = height, .grab_held = f < 240
		};
		sceneSetPointerOverride(scene, &pointer);
		const int grabbed = sceneGetGrabbedCount(scene);
		smoke->max_grabbed = grabbed > smoke->max_grabbed ? grabbed : smoke->max_grabbed;
	}
	else if (f == 260) {
		sceneSetPointerOverride(scene, NULL);
	}
	if (f == 300) {
		uiRequestReset(ui);
		smoke->resets++;
	}
	if (f == 360) {
		uiRequestBallSpawn(ui);
		uiRequestClothSpawn(ui);
	}
	return f < 600;
}

int main(int argc, const char*argv[]) {

	debugInstallExceptionHandler();
	debugSetPrintMask(DEBUG_PRINT_INFO | DEBUG_PRINT_WARNING | DEBUG_PRINT_ERROR);

	timerStartup();

	heap_t* heap = heapCreate(256 * 1024 * 1024); // 256 MBs
	bool smoke_mode = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--test") == 0) {
			const int failures = testPhysics(heap);
			heapDestroy(heap);
			return failures;
		}
		smoke_mode = smoke_mode || strcmp(argv[i], "--smoke") == 0;
	}
	wm_window_t* window = wmCreateWindow(heap);
	ui_t* ui = uiCreate(heap, window);
	fs_t* fs = fsCreate(heap, 8);
	timer_object_t* root_time = timerObjectCreate(heap, NULL);
	renderer_t* renderer = rendererCreate(heap, window);

	scene_t* scene = sceneCreate(heap, fs, window, renderer);

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
		const bool passed = smoke.frame >= 600 && smoke.max_grabbed > 0 && smoke.resets == 1;
		debugPrintConsole("smoke test %s: %d frames, grabbed up to %d particles, %d reset(s)\n",
			passed ? "passed" : "FAILED", smoke.frame, smoke.max_grabbed, smoke.resets);
		return passed ? 0 : 1;
	}
	return 0;
}
