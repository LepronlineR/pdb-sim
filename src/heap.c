#include "heap.h"
#include "debug.h"
#include "tlsf/tlsf.h"

#include <stdbool.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct object_t {
	struct object_t* next;
	bool marked;
	size_t size;
	void* backtrace[32];
	int backtrace_frames;
	void* data;
	pool_t pool;
} object_t;

typedef struct heap_t {
	tlsf_t tlsf;
	size_t grow_increment;
	object_t* object;
} heap_t;

heap_t* heapCreate(size_t grow_increment) {
	heap_t* heap = VirtualAlloc(NULL, sizeof(heap_t) + tlsf_size(),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (!heap) {
		return NULL;
	}

	heap->tlsf = tlsf_create(heap + 1);
	heap->grow_increment = grow_increment;
	heap->object = NULL;

	return heap;
}

void* heapAlloc(heap_t* heap, size_t size, size_t alignment) {
	void* address = tlsf_memalign(heap->tlsf, alignment, size);
	if (!address) {
		size_t object_size = __max(heap->grow_increment, size * 2) + sizeof(object_t);
		object_t* object = VirtualAlloc(NULL, object_size + tlsf_pool_overhead(),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!object) { // cannot allocate enough for the object 
			debugPrint(DEBUG_PRINT_ERROR, "Heap Allocation Error: unable to allocate enough memory for the object.\n");
			return NULL;
		}
		object->pool = tlsf_add_pool(heap->tlsf, object + 1, object_size);
		object->next = heap->object;
		heap->object = object;
		object->backtrace_frames = debugBacktrace(object->backtrace, 32);
		
		address = tlsf_memalign(heap->tlsf, alignment, size);

	} else { // heap did not allocate correctly...
		debugPrint(DEBUG_PRINT_ERROR, "Heap Allocation Error: unable to allocate enough memory.\n");
		return NULL;
	}
	return address;
}

void heapFree(heap_t* heap, void* address) {
	tlsf_free(heap->tlsf, address);
}

void heapDestroy(heap_t* heap) {
	tlsf_destroy(heap->tlsf);

	object_t* object = heap->object;
	object_t* next;
	while (object) {
		// check if the object has been freed by tlsf, otherwise return a backtrace of the leaked mem
		// 
		// NOTE: this has not been implemented yet, there is no obvious connection between the address and object
		//		 so TODO: create a map/hash when heap is freed, then we note it as deallocated
		// 
		// debugBacktraceLeakedMemory(object->backtrace, object->backtrace_frames);
		next = object->next;
		VirtualFree(object, 0, MEM_RELEASE);
		object = next;
	}

	VirtualFree(heap, 0, MEM_RELEASE);
}
