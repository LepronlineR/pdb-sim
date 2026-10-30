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
	/* Slots are allocated in a power of two at least length, so the running
	   head and tail counters map to slots with a mask. Unlike % on the signed
	   counters, this stays correct after they wrap past INT_MAX. */
	unsigned int slot_mask;
	/* head_idx is the next slot to pop from the front, tail_idx the next slot
	   to push to at the back: items are [head_idx, tail_idx). */
	unsigned int head_idx;
	unsigned int tail_idx;
	int cur_size;
	/* Held while an index is claimed and its slot written or read. Without it,
	   of two producers the second could finish first and let a consumer read
	   the first one's slot before it is written. The semaphores do the
	   blocking; this only covers a few instructions, so it spins. */
	int slot_lock;
} deque_t;

static void dequeLock(deque_t* deque) {
	while (atomicCompareAssign(&deque->slot_lock, 0, 1) != 0) {}
}

static void dequeUnlock(deque_t* deque) {
	atomicWrite(&deque->slot_lock, 0);
}

static void** dequeSlot(deque_t* deque, unsigned int index) {
	return &deque->items[index & deque->slot_mask];
}

deque_t* dequeCreate(heap_t* heap, int length) {
	deque_t* deque = heapAlloc(heap, sizeof(deque_t), 8);
	unsigned int slots = 1;
	while (slots < (unsigned int)length) slots <<= 1;
	deque->items = heapAlloc(heap, sizeof(void*) * slots, 8);
	deque->slot_mask = slots - 1;
	/* Capacity stays exactly length: the semaphores bound it, not the slots. */
	deque->used = semaphoreCreate(0, length);
	deque->free = semaphoreCreate(length, length);
	deque->heap = heap;
	deque->length = length;
	deque->head_idx = 0;
	deque->tail_idx = 0;
	deque->cur_size = 0;
	deque->slot_lock = 0;
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
	dequeLock(deque);
	*dequeSlot(deque, --deque->head_idx) = item;  // one slot before the front
	deque->cur_size++;
	dequeUnlock(deque);
	semaphoreRelease(deque->used);
}

void* dequePopFront(deque_t* deque) {
	semaphoreGet(deque->used);
	dequeLock(deque);
	void* item = *dequeSlot(deque, deque->head_idx++);
	deque->cur_size--;
	dequeUnlock(deque);
	semaphoreRelease(deque->free);
	return item;
}

void dequePushBack(deque_t* deque, void* item) {
	semaphoreGet(deque->free);
	dequeLock(deque);
	*dequeSlot(deque, deque->tail_idx++) = item;
	deque->cur_size++;
	dequeUnlock(deque);
	semaphoreRelease(deque->used);
}

void* dequePopBack(deque_t* deque) {
	semaphoreGet(deque->used);
	dequeLock(deque);
	void* item = *dequeSlot(deque, --deque->tail_idx);  // one slot before the tail
	deque->cur_size--;
	dequeUnlock(deque);
	semaphoreRelease(deque->free);
	return item;
}

void* dequePeekFront(deque_t* deque) {
	dequeLock(deque);
	void* item = *dequeSlot(deque, deque->head_idx);
	dequeUnlock(deque);
	return item;
}

int dequeGetCurrentSize(deque_t* deque) {
	return atomicRead(&deque->cur_size);
}