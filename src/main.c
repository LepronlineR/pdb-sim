#include "wm.h"
#include "heap.h"
#include "fs.h"
#include "test.h"
#include "debug.h"

int main(int argc, const char*argv[]) {

	debugInstallExceptionHandler();
	debugSetPrintMask(DEBUG_PRINT_INFO | DEBUG_PRINT_WARNING | DEBUG_PRINT_ERROR);

	heap_t* heap = heapCreate(2 * 1024 * 1024); // 2 MB pool
	fs_t* fs = fsCreate(heap, 16);

	wm_window_t* window = wmCreateWindow(heap);

	while (wmPumpWindow(window)) {
		// update scene
	}

	fsDestroy(fs);
	wmDestroyWindow(window);
	heapDestroy(heap);
	return 0;
}