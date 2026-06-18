#ifndef __BALL_H__
#define __BALL_H__

#include "vec3f.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ball_t ball_t;
typedef struct heap_t heap_t;
typedef struct physics_t physics_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;

typedef struct ball_info_t {
	vec3f_t position;
	vec3f_t color;
	float radius;
	float mass;
	float structural_compliance;
	float bending_compliance;
	float volume_compliance;
	float velocity_retention;
} ball_info_t;

ball_info_t ballInfoDefault(vec3f_t position, vec3f_t color, float mass);
bool ballCanCreate(physics_t* physics);
ball_t* ballCreate(heap_t* heap, physics_t* physics, const ball_info_t* info);
void ballDestroy(ball_t* ball);
void ballUpdateMesh(ball_t* ball);
gpu_mesh_info_t* ballGetMesh(ball_t* ball);
uint16_t ballGetMidpoint(vec3f_t* positions, int* vertex_count,
	uint16_t* edge_a, uint16_t* edge_b, uint16_t* edge_midpoint,
	int* edge_count, uint16_t a, uint16_t b);
void ballBuildMesh(ball_t* ball, vec3f_t color);
void ballBuildConstraints(ball_t* ball, physics_t* physics, const ball_info_t* info);

#endif
