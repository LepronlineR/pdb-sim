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

typedef struct ecs_component_t {
	void* data;
	char name[32];
	size_t size;
} ecs_component_t;

static ecs_entity_t false_entity = {
	.entity = -1,
	.sequence = -1
};

ecs_t* ecsCreate(heap_t* heap);
void ecsUpdate(ecs_t* ecs);
void ecsDestroy(ecs_t* ecs);

int ecsComponentRegister(ecs_t* ecs, const char* name, size_t size, size_t alignment); 
size_t ecsComponentGetTypeSize(ecs_t* ecs, int component_type);

ecs_entity_t ecsEntityAdd(ecs_t* ecs, uint64_t component_mask);
void ecsEntityRemove(ecs_t* ecs, ecs_entity_t ref, bool allow_pending_add);

// Determines if a entity reference points to a valid entity.
// If allow_pending_add is true, entities that are not fully spawned are considered valid.
bool ecsEntityValid(ecs_t* ecs, ecs_entity_t ref, bool allow_pending_add);

// Get the memory for a component on an entity.
// NULL is returned if the entity is not valid or the component_type is not present on the entity.
// If allow_pending_add is true, will return component data for not fully spawned entities.
void* ecsEntityGet(ecs_t* ecs, ecs_entity_t ref, int component_type, bool allow_pending_add);

// Creates a new entity query by component type mask.
ecs_query_t ecsQueryCreate(ecs_t* ecs, uint64_t mask);

// Determines if the query points at a valid entity.
bool ecsQueryValid(ecs_t* ecs, ecs_query_t* query);

// Advances the query to the next matching entity, if any.
void ecsQueryNext(ecs_t* ecs, ecs_query_t* query);

// Get data for a component on the entity referenced by the query, if any.
void* ecsQueryGetComponent(ecs_t* ecs, ecs_query_t* query, int component_type);

// Get a entity reference for the current query location.
ecs_entity_t ecsQueryGetEntity(ecs_t* ecs, ecs_query_t* query);

#endif