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

int main(int argc, const char*argv[]) {

	debugInstallExceptionHandler();
	debugSetPrintMask(DEBUG_PRINT_INFO | DEBUG_PRINT_WARNING | DEBUG_PRINT_ERROR);

	timerStartup();

	heap_t* heap = heapCreate(256 * 1024 * 1024); // 256 MBs
	wm_window_t* window = wmCreateWindow(heap);
	ui_t* ui = uiCreate(heap, window);
	fs_t* fs = fsCreate(heap, 8);
	timer_object_t* root_time = timerObjectCreate(heap, NULL);
	renderer_t* renderer = rendererCreate(heap, window);

	scene_t* scene = sceneCreate(heap, fs, window, renderer);

	while (wmPumpWindow(window)) {
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
	return 0;
}
