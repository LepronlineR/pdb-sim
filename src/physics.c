#include "physics.h"
#include "heap.h"

typedef struct physics_t {
	void** data;
	float acceleration;
} physics_t;


physics_t* physicsCreate(heap_t* heap) {
	physics_t* phys = heapAlloc(heap, sizeof(physics_t), 8);
	phys->acceleration = 9.81f;

	return phys;
}