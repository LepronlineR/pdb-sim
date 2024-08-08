#include "wm.h"
#include "heap.h"
#include "fs.h"
#include "timer_object.h"
#include "timer.h"

#include "test.h"
#include "debug.h"

int main(int argc, const char*argv[]) {

	debugInstallExceptionHandler();
	debugSetPrintMask(DEBUG_PRINT_INFO | DEBUG_PRINT_WARNING | DEBUG_PRINT_ERROR);

	timerStartup();

	heap_t* heap = heapCreate(2 * 1024 * 1024); // 2 MB pool
	wm_window_t* window = wmCreateWindow(heap);
	fs_t* fs = fsCreate(heap, 8);
	timer_object_t* root_time = timerObjectCreate(heap, NULL);
	

	while (wmPumpWindow(window)) {
		// update scene
	}

	timerObjectDestroy(root_time);
	fsDestroy(fs);
	wmDestroyWindow(window);
	heapDestroy(heap);
	return 0;
}