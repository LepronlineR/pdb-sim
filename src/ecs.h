#ifndef __ENTITY_COMPONENT_SYSTEM_H__
#define __ENTITY_COMPONENT_SYSTEM_H__

#include <stdint.h>
#include <stdbool.h>

typedef struct ecs_t ecs_t;

// MISC
typedef struct heap_t heap_t;

typedef struct ecs_entity_t {
	int entity;
	int sequence;
} ecs_entity_t;

// Holds the data for an entity queries
typedef struct ecs_query_t {
	uint64_t component_mask;
	int entity;
} ecs_query_t;

typedef struct ecs_entity_dense_t {
	char name[32];
	void* data;
} ecs_entity_dense_t;

typedef struct ecs_entity_sparse_t {
	

} ecs_entity_dense_t;

static ecs_entity_t false_entity = {
	.entity = -1,
	.sequence = -1
};

ecs_t* ecsCreate(heap_t* heap);
void ecsUpdate(ecs_t* ecs);
void ecsDestroy(ecs_t* ecs);




#endif