#include "wm.h"
#include "heap.h"

int main(int argc, const char* argv[]) {

	heap_t* heap = heapCreate(2 * 1024 * 1024); // 2 MB pool
	wm_window_t* window = wmCreateWindow(heap);

	while (wmPumpWindow(window)) {

	}

	heapDestroy(heap);

	return 0;
}