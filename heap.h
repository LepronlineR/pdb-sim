#pragma once

typedef struct heap_t heap_t;

// TLSF includes constant time allocation and deallocation, low memory overhaul and low fragmentation'
// this is more suitable for real-time simulations, especially constant updates, due to the requirement
// of performance

// Creates a new memory heap.
// The grow increment is the default size with which the heap grows.
// Should be a multiple of OS page size.
heap_t* heapCreate(size_t grow_increment);

// Destroy a previously created heap.
void heapDestroy(heap_t* heap);

// Allocate memory from a heap.
void* heapAlloc(heap_t* heap, size_t size, size_t alignment);

// Free memory previously allocated from a heap.
void heapFree(heap_t* heap, void* address);