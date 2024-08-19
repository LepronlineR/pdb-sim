#include "ecs.h"
#include "heap.h"
#include "debug.h"

#define MAX_COMPONENT_TYPES 128
#define MAX_ENTITIES_ALLOWED 1024

typedef enum ecs_entity_state_t {
	ECS_ENTITY_INACTIVE,
	ECS_ENTITY_ADD,
	ECS_ENTITY_ACTIVE,
	ECS_ENTITY_REMOVE,
} ecs_entity_state_t;

typedef struct ecs_t {
	heap_t* heap;
	int count;

	int sequences[MAX_COMPONENT_TYPES];
	ecs_entity_state_t entity_states[MAX_ENTITIES_ALLOWED];
	uint64_t components_mask[MAX_COMPONENT_TYPES];
	ecs_component_t components[MAX_COMPONENT_TYPES];
};

ecs_t* ecsCreate(heap_t* heap) {
	ecs_t* ecs = heapAlloc(heap, sizeof(ecs_t), 8);
	memset(ecs, 0, sizeof(ecs));
	ecs->heap = heap;
	ecs->count = 1;
	return ecs;
}

void ecsDestroy(ecs_t* ecs) {
	for (int x = 0; x < _countof(ecs->components); x++) {
		if (ecs->components[x].data) {
			heapFree(ecs->heap, ecs->components[x].data);
		}
	}
	heapFree(ecs->heap, ecs);
}

void ecsUpdate(ecs_t* ecs) {
	for (int x = 0; x < _countof(ecs->entity_states); x++) {
		if (ecs->entity_states[x] == ECS_ENTITY_ADD) {
			ecs->entity_states[x] = ECS_ENTITY_ACTIVE;
		} else if (ecs->entity_states[x] == ECS_ENTITY_REMOVE) {
			ecs->entity_states[x] = ECS_ENTITY_REMOVE;
		}
	}
}

int ecsComponentRegister(ecs_t* ecs, const char* name, size_t size, size_t alignment) {
	for (int x = 0; x < _countof(ecs->components); x++) {
		if (!ecs->components[x].data) {
			size_t align = (size + (alignment - 1)) & ~(alignment - 1);
			ecs->components[x] = (ecs_component_t){
				.data = heapAlloc(ecs->heap, align * MAX_ENTITIES_ALLOWED, alignment),
				.size = align
			};
			// sizeof(ecs->components[x].name) --> 32 * 1 (32 char)
			strcpy_s(ecs->components[x].name, 32, name);
			return x;
		}
	}
	debugPrint(DEBUG_PRINT_ERROR, "Ecs Component Register: Unable to register a component, out of types.");
	return -1;
}

size_t ecsComponentGetTypeSize(ecs_t* ecs, int component_type) {
	return ecs->components[component_type].size;
}

ecs_entity_t ecsEntityAdd(ecs_t* ecs, uint64_t component_mask) {
	for (int x = 0; x < _countof(ecs->entity_states); x++) {
		if (ecs->entity_states[x] == ECS_ENTITY_INACTIVE) {
			ecs->entity_states[x] = ECS_ENTITY_ADD;
			ecs->sequences[x] = ecs->count++;
			ecs->components_mask[x] = component_mask;
			return (ecs_entity_t) {
				.entity = x,
				.sequence = ecs->sequences[x]
			};
		}
	}
	debugPrint(DEBUG_PRINT_ERROR, "Unable to add an entity.");
	return false_entity;
}

void ecsEntityRemove(ecs_t* ecs, ecs_entity_t ref, bool allow_pending_add) {
	if (ecsEntityValid(ecs, ref, allow_pending_add)) {
		ecs->entity_states[ref.entity] = ECS_ENTITY_REMOVE;
	} else {
		debugPrint(DEBUG_PRINT_WARNING, "Ecs Entity Remove: Trying to remove an entity that is already not active.");
	}
}

bool ecsEntityValid(ecs_t* ecs, ecs_entity_t ref, bool allow_pending_add) {
	return ref.entity >= 0 && ecs->sequences[ref.entity] == ref.sequence
		&& ecs->entity_states[ref.entity] >= (allow_pending_add ? ECS_ENTITY_ADD : ECS_ENTITY_ACTIVE);
}

void* ecsEntityGet(ecs_t* ecs, ecs_entity_t ref, int component_type, bool allow_pending_add) {
	return ecsEntityValid(ecs, ref, allow_pending_add) ? ecs->components[component_type].data : NULL;
}

ecs_query_t ecsQueryCreate(ecs_t* ecs, uint64_t mask) {
	ecs_query_t query = {
		.component_mask = mask,
		.entity = -1
	};
	ecsQueryNext(ecs, &query);
	return query;
}

bool ecsQueryValid(ecs_t* ecs, ecs_query_t* query) {
	return query->entity >= 0;
}

void ecsQueryNext(ecs_t* ecs, ecs_query_t* query) {
	for (int x = query->entity + 1; x < _countof(ecs->components_mask); x++) {
		if ((ecs->components_mask[x] & query->component_mask) == 
			(query->component_mask && ecs->entity_states[x] >= ECS_ENTITY_ACTIVE)) {
			query->entity = x;
			return;
		}
	}
	debugPrint(DEBUG_PRINT_WARNING, "Unable to get the next query.");
	query->entity = -1;
}

void* ecsQueryGetComponent(ecs_t* ecs, ecs_query_t* query, int component_type) {
	return ecs->components[ecs->components[component_type].size * query->entity].data;
}

ecs_entity_t ecsQueryGetEntity(ecs_t* ecs, ecs_query_t* query) {
	return (ecs_entity_t) {
		.entity = query->entity,
		.sequence = ecs->sequences[query->entity]
	};
}