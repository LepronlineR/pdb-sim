#include "wm.h"
#include "heap.h"
#include "test.h"
#include "debug.h"

int main(int argc, const char* argv[]) {

	debugSetPrintMask(DEBUG_PRINT_INFO, DEBUG_PRINT_WARNING, DEBUG_PRINT_ERROR);
	debugInstallExceptionHandler();

	heap_t* heap = heapCreate(2 * 1024 * 1024); // 2 MB pool
	wm_window_t* window = wmCreateWindow(heap);

	testLeakedHeapAllocation();

	while (wmPumpWindow(window)) {
		// update scene
	}

	wmDestroyWindow(window);
	heapDestroy(heap);
	return 0;
}