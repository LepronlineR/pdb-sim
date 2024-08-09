#include "ecs.h"
#include "heap.h"

typedef struct ecs_t {
	heap_t* heap;

};

ecs_t* ecsCreate(heap_t* heap) {
	ecs_t* ecs = heapAlloc(heap, sizeof(ecs_t), 8);
}

void ecsUpdate(ecs_t* ecs);
void ecsDestroy(ecs_t* ecs);
