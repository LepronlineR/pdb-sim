#include "deque.h"
#include "semaphore.h"
#include "atomic.h"
#include "heap.h"

typedef struct deque_t {
	heap_t* heap;
	semaphore_t* used;
	semaphore_t* free;
	void** items;
	int length;
	int head_idx;
	int tail_idx;
} deque_t;

deque_t* dequeCreate(heap_t* heap, int length) {
	deque_t* deque = heapAlloc(heap, sizeof(deque_t), 8);
	deque->items = heapAlloc(heap, sizeof(void*) * length, 8);
	deque->used = semaphoreCreate(0, length);
	deque->free = semaphoreCreate(length, length);
	deque->heap = heap;
	deque->length = length;
	deque->head_idx = 0;
	deque->tail_idx = 0;
	return deque;
}

void dequeDestroy(deque_t* deque) {
	semaphoreDestroy(deque->free);
	semaphoreDestroy(deque->used);
	heapFree(deque->heap, deque->items);
	heapFree(deque->heap, deque);
}

void dequePushFront(deque_t* deque, void* item) {
	semaphoreGet(deque->free);
	int new_head_idx = atomicDec(&deque->head_idx) % deque->length;
	deque->items[new_head_idx] = item;
	semaphoreRelease(deque->used);
}

void* dequePopFront(deque_t* deque) {
	semaphoreGet(deque->used);
	int new_head_idx = atomicInc(&deque->head_idx) % deque->length;
	void* item = deque->items[new_head_idx];
	semaphoreRelease(deque->free);
	return item;
}

void dequePushBack(deque_t* deque, void* item) {
	semaphoreGet(deque->free);
	int new_tail_idx = atomicInc(&deque->tail_idx) % deque->length;
	deque->items[new_tail_idx] = item;
	semaphoreRelease(deque->used);
}

void* dequePopBack(deque_t* deque) {
	semaphoreGet(deque->used);
	int new_tail_idx = atomicDec(&deque->tail_idx) % deque->length;
	void* item = deque->items[new_tail_idx];
	semaphoreRelease(deque->free);
	return item;
}
