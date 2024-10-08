#ifndef __HEAP_H__
#define __HEAP_H__

#include <stdlib.h>

typedef struct heap_t heap_t;

// TLSF includes constant time allocation and deallocation, low memory overhaul and low fragmentation'
// this is more suitable for real-time simulations, especially constant updates, due to the requirement
// of performance

// Creates a new memory heap.
// The grow increment is the default size with which the heap grows.
// Should be a multiple of OS page size.
//
// RETURN: the new memory heap
heap_t* heapCreate(size_t grow_increment);

// Destroy a previously created heap.
// - For each object in the heap, check if there are any leaks and report them to the user as a callstack.
void heapDestroy(heap_t* heap);

// Allocate memory from a heap.
//
// RETURN: the address of the new allocated memory
void* heapAlloc(heap_t* heap, size_t size, size_t alignment);

// Free memory previously allocated from a heap.
void heapFree(heap_t* heap, void* address);

#endif