#ifndef __PHYSICS_H__
#define __PHYSICS_H__



typedef struct physics_t physics_t;

typedef struct heap_t heap_t;
typedef struct ecs_t ecs_t;


physics_t* physicsCreate(heap_t* heap);

void physicsUpdate(physics_t* physics);


#endif