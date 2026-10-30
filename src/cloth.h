#ifndef __CLOTH_H__
#define __CLOTH_H__

#include "physics.h"
#include "vec3f.h"
#include <stdbool.h>
#include <stdint.h>

/*		CLOTH
*
*	A square sheet of particles laid out flat (in the xz plane) and held together by
*	stretch, shear and bending distance constraints. It collides with itself and with
*	other soft bodies, and is drawn double-sided.
*/

typedef struct cloth_t cloth_t;
typedef struct heap_t heap_t;
typedef struct physics_t physics_t;
typedef struct gpu_mesh_info_t gpu_mesh_info_t;

typedef enum cloth_pin_t {
	CLOTH_PIN_NONE,
	CLOTH_PIN_CORNERS,  // all four corners: a trampoline
	CLOTH_PIN_EDGE,     // one edge: a hanging banner
	CLOTH_PIN_BORDER,   // the whole border: a drum or trampoline bed
	CLOTH_PIN_COUNT
} cloth_pin_t;

typedef struct cloth_info_t {
	vec3f_t center;
	vec3f_t color;
	int resolution;          // particles per side
	float size;              // side length in meters
	float mass;
	float stretch_compliance;
	float shear_compliance;
	float bend_compliance;
	float velocity_retention;
	float drag;              // fraction of velocity lost per second to the air
	cloth_pin_t pin;
} cloth_info_t;

cloth_info_t clothInfoDefault(vec3f_t center, vec3f_t color);
bool clothCanCreate(physics_t* physics, const cloth_info_t* info);
cloth_t* clothCreate(heap_t* heap, physics_t* physics, const cloth_info_t* info);
void clothDestroy(cloth_t* cloth);
void clothUpdateMesh(cloth_t* cloth);
gpu_mesh_info_t* clothGetMesh(cloth_t* cloth);
// The cloth's particles are [first_particle, first_particle + particle_count).
void clothGetParticles(cloth_t* cloth, physics_particle_id_t* first_particle,
	int* particle_count);

#endif
